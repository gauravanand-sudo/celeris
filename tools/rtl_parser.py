"""
rtl_parser.py — Verilog subset parser for Celeris RTL Simulator.

Parses signals (input/output/inout/wire/reg) and processes
(assign statements and always blocks) from a Verilog module.
Reports syntax errors with line numbers; non-fatal issues as warnings.
"""

import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple


@dataclass
class Signal:
    name: str
    kind: str    # input, output, inout, wire, reg
    width: int = 1


@dataclass
class Process:
    name: str
    kind: str    # sequential, combinational
    sensitivity: List[str]
    drives: List[str]
    reads: List[str]


@dataclass
class ParsedModule:
    name: str
    signals: Dict[str, Signal]
    processes: List[Process]
    errors: List[str]
    warnings: List[str] = field(default_factory=list)


# Keywords and tokens that are not signal identifiers
_KEYWORDS = frozenset({
    'module', 'endmodule', 'input', 'output', 'inout', 'wire', 'reg',
    'always', 'assign', 'begin', 'end', 'if', 'else', 'case', 'casez',
    'casex', 'endcase', 'posedge', 'negedge', 'or', 'and', 'not',
    'for', 'while', 'repeat', 'forever', 'initial', 'parameter',
    'localparam', 'integer', 'real', 'time', 'default', 'function',
    'endfunction', 'task', 'endtask', 'generate', 'endgenerate',
    'genvar', 'signed', 'unsigned', 'automatic', 'logic',
})


def _line_of(code: str, pos: int) -> int:
    """Return 1-based line number for character position."""
    return code[:pos].count('\n') + 1


def _strip_comments(code: str) -> Tuple[str, Dict[int, int]]:
    """
    Remove // and /* */ comments. Returns cleaned code and a char-to-original-line map.
    Preserves newlines so line numbers stay accurate.
    """
    result = []
    i = 0
    length = len(code)
    while i < length:
        if i + 1 < length and code[i] == '/' and code[i + 1] == '*':
            i += 2
            while i + 1 < length and not (code[i] == '*' and code[i + 1] == '/'):
                if code[i] == '\n':
                    result.append('\n')
                i += 1
            i += 2
        elif i + 1 < length and code[i] == '/' and code[i + 1] == '/':
            i += 2
            while i < length and code[i] != '\n':
                i += 1
        else:
            result.append(code[i])
            i += 1
    return ''.join(result)


def _parse_width(msb: str, lsb: str) -> int:
    try:
        return int(msb) - int(lsb) + 1
    except (ValueError, TypeError):
        return 1


def _extract_identifiers(expr: str) -> List[str]:
    raw = re.findall(r'\b([A-Za-z_][A-Za-z0-9_$]*)\b', expr)
    seen = set()
    result = []
    for name in raw:
        if name not in _KEYWORDS and name not in seen:
            seen.add(name)
            result.append(name)
    return result


def _extract_lhs_names(stmt: str) -> List[str]:
    names = []
    for m in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_$]*)\b\s*(?:\[[^\]]*\])?\s*(?:<=|=)', stmt):
        name = m.group(1)
        if name not in _KEYWORDS:
            names.append(name)
    return names


def _parse_sensitivity_list(sens_str: str) -> Tuple[str, List[str]]:
    sens_str = sens_str.strip()
    if sens_str == '*':
        return 'combinational', []

    kind = 'combinational'
    signals = []
    tokens = re.split(r'\bor\b|,', sens_str, flags=re.IGNORECASE)
    for tok in tokens:
        tok = tok.strip()
        if re.match(r'posedge\b', tok, re.IGNORECASE):
            kind = 'sequential'
            m = re.search(r'posedge\s+(\w+)', tok, re.IGNORECASE)
            if m:
                signals.append(m.group(1))
        elif re.match(r'negedge\b', tok, re.IGNORECASE):
            kind = 'sequential'
            m = re.search(r'negedge\s+(\w+)', tok, re.IGNORECASE)
            if m:
                signals.append(m.group(1))
        else:
            idents = _extract_identifiers(tok)
            signals.extend(idents)
    return kind, signals


def _find_always_blocks(code: str) -> List[Tuple[str, str, int]]:
    """
    Find all always blocks. Returns list of (sensitivity_string, body_string, line_no).
    """
    results = []
    always_pat = re.compile(r'\balways\s*@\s*\(', re.DOTALL)

    pos = 0
    while pos < len(code):
        m = always_pat.search(code, pos)
        if not m:
            break

        line_no = _line_of(code, m.start())
        paren_start = m.end() - 1
        depth = 0
        i = paren_start
        while i < len(code):
            if code[i] == '(':
                depth += 1
            elif code[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        sens_content = code[paren_start + 1:i]
        body_start = i + 1

        j = body_start
        while j < len(code) and code[j] in ' \t\n\r':
            j += 1

        if code[j:j + 5] == 'begin':
            k = j + 5
            begin_depth = 1
            body_end = -1
            while k < len(code) and begin_depth > 0:
                if code[k:k + 9] == 'endmodule':
                    break
                if code[k:k + 7] == 'endcase':
                    k += 7
                    continue
                if code[k:k + 5] == 'begin':
                    begin_depth += 1
                    k += 5
                    continue
                if code[k:k + 3] == 'end' and (k + 3 >= len(code) or not code[k + 3].isalnum() and code[k + 3] != '_'):
                    begin_depth -= 1
                    if begin_depth == 0:
                        body_end = k + 3
                        break
                    k += 3
                    continue
                k += 1
            if body_end == -1:
                body_end = k
            body = code[j + 5:body_end - 3].strip()
        else:
            k = j
            while k < len(code) and code[k] != ';':
                k += 1
            body = code[j:k + 1].strip()
            body_end = k + 1

        results.append((sens_content, body, line_no))
        pos = body_end if body_end > m.start() else m.end()

    return results


def _parse_signals(code: str) -> Dict[str, Signal]:
    signals: Dict[str, Signal] = {}

    mod_paren_m = re.search(r'\bmodule\b[^(]*\(', code, re.DOTALL)
    port_list_end = 0
    if mod_paren_m:
        start = mod_paren_m.end() - 1
        depth = 0
        i = start
        while i < len(code):
            if code[i] == '(':
                depth += 1
            elif code[i] == ')':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        port_list_content = code[start + 1:i]
        port_list_end = i + 1

        entries = _split_port_entries(port_list_content)
        for entry in entries:
            entry = entry.strip()
            if not entry:
                continue
            sig = _parse_single_port_decl(entry)
            if sig and sig.name not in signals:
                signals[sig.name] = sig

    body_code = code[port_list_end:]
    decl_pat = re.compile(
        r'\b(input|output|inout|wire|reg)\b'
        r'(?:\s+reg\b)?'
        r'\s*(?:\[\s*(\d+)\s*:\s*(\d+)\s*\])?'
        r'\s*'
        r'([A-Za-z_][A-Za-z0-9_$]*(?:\s*,\s*[A-Za-z_][A-Za-z0-9_$]*)*)'
        r'\s*(?:[=;])',
    )
    for m in decl_pat.finditer(body_code):
        kind = m.group(1)
        msb_s, lsb_s = m.group(2), m.group(3)
        width = _parse_width(msb_s, lsb_s) if msb_s is not None else 1
        names_str = m.group(4)
        for raw_name in names_str.split(','):
            raw_name = raw_name.strip()
            id_m = re.match(r'^([A-Za-z_][A-Za-z0-9_$]*)', raw_name)
            if id_m:
                name = id_m.group(1)
                if name not in _KEYWORDS and name not in signals:
                    signals[name] = Signal(name=name, kind=kind, width=width)

    return signals


def _split_port_entries(port_list: str) -> List[str]:
    entries = []
    depth = 0
    current = []
    for ch in port_list:
        if ch == '[':
            depth += 1
            current.append(ch)
        elif ch == ']':
            depth -= 1
            current.append(ch)
        elif ch == ',' and depth == 0:
            entries.append(''.join(current))
            current = []
        else:
            current.append(ch)
    if current:
        entries.append(''.join(current))
    return entries


def _parse_single_port_decl(entry: str) -> Optional[Signal]:
    entry = entry.strip()
    if not entry:
        return None

    m = re.match(
        r'^(input|output|inout|wire|reg)\b'
        r'(?:\s+reg\b)?'
        r'\s*(?:\[\s*(\d+)\s*:\s*(\d+)\s*\])?'
        r'\s*([A-Za-z_][A-Za-z0-9_$]*)',
        entry,
    )
    if m:
        kind = m.group(1)
        msb_s, lsb_s = m.group(2), m.group(3)
        name = m.group(4)
        width = _parse_width(msb_s, lsb_s) if msb_s is not None else 1
        if name not in _KEYWORDS:
            return Signal(name=name, kind=kind, width=width)

    id_m = re.match(r'^([A-Za-z_][A-Za-z0-9_$]*)', entry)
    if id_m:
        name = id_m.group(1)
        if name not in _KEYWORDS:
            return Signal(name=name, kind='wire', width=1)

    return None


# ── Syntax checks ─────────────────────────────────────────────────────────────

def _check_bracket_balance(code: str, errors: List[str]):
    """Check balanced parentheses and brackets (ignoring string literals)."""
    paren, bracket = 0, 0
    paren_line = bracket_line = 1
    line = 1
    for ch in code:
        if ch == '\n':
            line += 1
        elif ch == '(':
            if paren == 0:
                paren_line = line
            paren += 1
        elif ch == ')':
            paren -= 1
            if paren < 0:
                errors.append(f"Line {line}: unexpected ')' — no matching '('")
                paren = 0
        elif ch == '[':
            if bracket == 0:
                bracket_line = line
            bracket += 1
        elif ch == ']':
            bracket -= 1
            if bracket < 0:
                errors.append(f"Line {line}: unexpected ']' — no matching '['")
                bracket = 0
    if paren > 0:
        errors.append(f"Line {paren_line}: unclosed '(' — missing closing ')'")
    if bracket > 0:
        errors.append(f"Line {bracket_line}: unclosed '[' — missing closing ']'")


def _check_begin_end_balance(code: str, errors: List[str]):
    """Check that every begin has a matching end."""
    depth = 0
    begin_line = 1
    line = 1

    i = 0
    while i < len(code):
        if code[i] == '\n':
            line += 1
            i += 1
            continue

        # endmodule / endcase / endfunction / endtask  — skip, not a bare 'end'
        for keyword in ('endmodule', 'endcase', 'endfunction', 'endtask',
                        'endgenerate', 'endtable'):
            if code[i:i + len(keyword)] == keyword:
                tail = code[i + len(keyword):i + len(keyword) + 1]
                if not tail or not (tail.isalnum() or tail == '_'):
                    i += len(keyword)
                    break
        else:
            if code[i:i + 5] == 'begin':
                tail = code[i + 5:i + 6]
                if not tail or not (tail.isalnum() or tail == '_'):
                    if depth == 0:
                        begin_line = line
                    depth += 1
                    i += 5
                    continue
            elif code[i:i + 3] == 'end':
                tail = code[i + 3:i + 4]
                if not tail or not (tail.isalnum() or tail == '_'):
                    depth -= 1
                    if depth < 0:
                        errors.append(f"Line {line}: unexpected 'end' — no matching 'begin'")
                        depth = 0
                    i += 3
                    continue
            i += 1
            continue
        # fell through from inner break
        continue

    if depth > 0:
        errors.append(f"Line {begin_line}: unclosed 'begin' — missing 'end' ({depth} level{'s' if depth>1 else ''} unclosed)")


def _check_always_syntax(code: str, errors: List[str]):
    """Check always blocks have @(...) sensitivity lists."""
    for m in re.finditer(r'\balways\b', code):
        line = _line_of(code, m.start())
        rest = code[m.end():m.end() + 20].strip()
        if not rest.startswith('@'):
            errors.append(
                f"Line {line}: 'always' without sensitivity list — expected 'always @(...)' or 'always @*'"
            )


def _check_assign_syntax(code: str, errors: List[str]):
    """Check assign statements have a LHS signal and '='."""
    for m in re.finditer(r'\bassign\b', code):
        line = _line_of(code, m.start())
        # Find the next semicolon
        end = code.find(';', m.end())
        if end == -1:
            errors.append(f"Line {line}: 'assign' statement missing terminating ';'")
            continue
        stmt = code[m.end():end]
        if '=' not in stmt:
            errors.append(f"Line {line}: 'assign' statement missing '=' — expected 'assign signal = expr;'")


def _check_output_never_driven(signals: Dict[str, Signal], processes: List[Process], warnings: List[str]):
    """Warn if output/reg signals are never driven by any process."""
    driven_by_process: set = set()
    for p in processes:
        driven_by_process.update(p.drives)

    for name, sig in signals.items():
        if sig.kind == 'output' and name not in driven_by_process:
            warnings.append(
                f"Signal '{name}' declared as output but never assigned in any always block or assign statement"
            )


def _check_undeclared_reads(signals: Dict[str, Signal], processes: List[Process], warnings: List[str]):
    """Warn if a process reads a signal that is not declared."""
    declared = set(signals.keys())
    for p in processes:
        for r in p.reads:
            if r not in declared:
                warnings.append(f"Process '{p.name}' references undeclared identifier '{r}'")


def _check_multiple_drivers(processes: List[Process], warnings: List[str]):
    """Warn if the same signal is driven by more than one always block."""
    driven_by: Dict[str, List[str]] = {}
    for p in processes:
        for d in p.drives:
            driven_by.setdefault(d, []).append(p.name)
    for sig, procs in driven_by.items():
        if len(procs) > 1:
            warnings.append(
                f"Signal '{sig}' is driven by multiple processes ({', '.join(procs)}) — potential multiple-driver conflict"
            )


# ── Public API ────────────────────────────────────────────────────────────────

def parse(verilog: str) -> ParsedModule:
    """
    Parse a Verilog module from source text.
    Returns a ParsedModule with signals, processes, errors, and warnings.
    """
    errors: List[str] = []
    warnings: List[str] = []

    code = _strip_comments(verilog)

    # ── 1. Module declaration ──────────────────────────────────────
    mod_m = re.search(r'\bmodule\s+(\w+)', code)
    if not mod_m:
        errors.append("No 'module' declaration found — Verilog must start with 'module <name>(...);'")
        return ParsedModule(name='unknown', signals={}, processes=[], errors=errors, warnings=warnings)

    module_name = mod_m.group(1)

    if 'endmodule' not in code:
        errors.append("Missing 'endmodule' — module is not closed")
        # continue parsing for more errors

    # ── 2. Structural checks (syntax before parsing) ───────────────
    _check_bracket_balance(code, errors)
    _check_begin_end_balance(code, errors)
    _check_always_syntax(code, errors)
    _check_assign_syntax(code, errors)

    # If hard structural errors found, stop before parsing
    if errors:
        # still try to parse signals for a partial result
        signals = _parse_signals(code)
        return ParsedModule(name=module_name, signals=signals, processes=[], errors=errors, warnings=warnings)

    # ── 3. Parse signals ───────────────────────────────────────────
    signals = _parse_signals(code)

    if not signals:
        errors.append(
            "No signals found — check port declarations "
            "(input/output/wire/reg) are properly listed"
        )

    # ── 4. Parse processes ─────────────────────────────────────────
    processes: List[Process] = []
    proc_counter = {'assign': 0, 'always': 0}

    assign_pat = re.compile(
        r'\bassign\s+([A-Za-z_][A-Za-z0-9_$]*)\s*(?:\[[^\]]*\])?\s*=\s*([^;]+);',
        re.DOTALL
    )
    for m in assign_pat.finditer(code):
        lhs_name = m.group(1)
        rhs_expr = m.group(2)
        proc_counter['assign'] += 1
        proc_name = f'assign_{proc_counter["assign"]}_{lhs_name}'

        reads = _extract_identifiers(rhs_expr)
        drives = [lhs_name] if lhs_name not in _KEYWORDS else []

        processes.append(Process(
            name=proc_name,
            kind='combinational',
            sensitivity=reads,
            drives=drives,
            reads=reads,
        ))
        if lhs_name not in signals and lhs_name not in _KEYWORDS:
            signals[lhs_name] = Signal(name=lhs_name, kind='wire', width=1)

    always_blocks = _find_always_blocks(code)
    for _, (sens_str, body, line_no) in enumerate(always_blocks):
        proc_counter['always'] += 1
        kind, sensitivity = _parse_sensitivity_list(sens_str)

        if not sensitivity and sens_str.strip() != '*':
            warnings.append(
                f"Line {line_no}: always block has empty sensitivity list — "
                "did you mean 'always @(*)'?"
            )

        all_idents = _extract_identifiers(body)
        drives = _extract_lhs_names(body)
        seen_drives = set()
        unique_drives = []
        for d in drives:
            if d not in seen_drives:
                seen_drives.add(d)
                unique_drives.append(d)
        drives = unique_drives

        reads = [i for i in all_idents if i not in seen_drives or i in sensitivity]
        if sens_str.strip() == '*':
            sensitivity = list(reads)

        proc_name = f'always_{proc_counter["always"]}'
        if drives:
            proc_name = f'always_{proc_counter["always"]}_{drives[0]}'

        processes.append(Process(
            name=proc_name,
            kind=kind,
            sensitivity=sensitivity,
            drives=drives,
            reads=reads,
        ))

    if not processes:
        errors.append(
            "No processes found — no 'always @(...)' blocks or 'assign' statements detected"
        )

    # ── 5. Semantic checks (warnings) ─────────────────────────────
    if not errors:
        _check_output_never_driven(signals, processes, warnings)
        _check_multiple_drivers(processes, warnings)
        _check_undeclared_reads(signals, processes, warnings)

    return ParsedModule(
        name=module_name,
        signals=signals,
        processes=processes,
        errors=errors,
        warnings=warnings,
    )
