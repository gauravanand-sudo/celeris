"""
rtl_parser.py — Verilog subset parser for Celeris RTL Simulator.

Parses signals (input/output/inout/wire/reg) and processes
(assign statements and always blocks) from a Verilog module.
"""

import re
from dataclasses import dataclass
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


def _strip_comments(code: str) -> str:
    """Remove // line comments and /* */ block comments from Verilog source."""
    result = []
    i = 0
    length = len(code)
    while i < length:
        # Block comment
        if i + 1 < length and code[i] == '/' and code[i + 1] == '*':
            i += 2
            while i + 1 < length and not (code[i] == '*' and code[i + 1] == '/'):
                if code[i] == '\n':
                    result.append('\n')
                i += 1
            i += 2  # skip */
        # Line comment
        elif i + 1 < length and code[i] == '/' and code[i + 1] == '/':
            i += 2
            while i < length and code[i] != '\n':
                i += 1
        else:
            result.append(code[i])
            i += 1
    return ''.join(result)


def _parse_width(msb: str, lsb: str) -> int:
    """Compute bit-width from MSB and LSB strings."""
    try:
        return int(msb) - int(lsb) + 1
    except (ValueError, TypeError):
        return 1


def _extract_identifiers(expr: str) -> List[str]:
    """Extract all valid Verilog identifiers from an expression, excluding keywords."""
    raw = re.findall(r'\b([A-Za-z_][A-Za-z0-9_$]*)\b', expr)
    seen = set()
    result = []
    for name in raw:
        if name not in _KEYWORDS and name not in seen:
            seen.add(name)
            result.append(name)
    return result


def _extract_lhs_names(stmt: str) -> List[str]:
    """
    Extract signal names from the LHS of blocking/non-blocking assignments.
    Handles: name, name[idx], name[msb:lsb].
    """
    names = []
    for m in re.finditer(r'\b([A-Za-z_][A-Za-z0-9_$]*)\b\s*(?:\[[^\]]*\])?\s*(?:<=|=)', stmt):
        name = m.group(1)
        if name not in _KEYWORDS:
            names.append(name)
    return names


def _parse_sensitivity_list(sens_str: str) -> Tuple[str, List[str]]:
    """
    Parse an always @(...) sensitivity list.
    Returns (kind, [signal_names]).
    kind is 'sequential' if posedge/negedge present, 'combinational' otherwise.
    """
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


def _find_always_blocks(code: str) -> List[Tuple[str, str]]:
    """
    Find all always blocks in the code.
    Returns list of (sensitivity_list_string, body_string) tuples.
    Uses character-by-character scanning for balanced begin/end matching,
    carefully skipping endmodule and endcase.
    """
    results = []
    # Match always @(...)
    always_pat = re.compile(r'\balways\s*@\s*\(', re.DOTALL)

    pos = 0
    while pos < len(code):
        m = always_pat.search(code, pos)
        if not m:
            break

        # Extract sensitivity list between the opening paren (already consumed) and matching close paren
        paren_start = m.end() - 1  # position of '('
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
        body_start = i + 1  # after the closing ')'

        # Skip whitespace to find begin or first statement
        j = body_start
        while j < len(code) and code[j] in ' \t\n\r':
            j += 1

        # Check if body starts with 'begin'
        if code[j:j + 5] == 'begin':
            # Find matching end using character scan, skipping endmodule/endcase
            k = j + 5
            begin_depth = 1
            body_end = -1
            while k < len(code) and begin_depth > 0:
                # Check for endmodule or endcase (do NOT count as end)
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
            # Single statement: read until semicolon
            k = j
            while k < len(code) and code[k] != ';':
                k += 1
            body = code[j:k + 1].strip()
            body_end = k + 1

        results.append((sens_content, body))
        pos = body_end if body_end > m.start() else m.end()

    return results


def _parse_signals(code: str) -> Dict[str, Signal]:
    """
    Parse all signal declarations from the Verilog code.

    Two passes:
    1. Module header port list: extract individual port declarations from the
       parenthesised argument list of the module statement. Each port is a
       comma-separated entry like 'input [7:0] a' or 'output reg [8:0] result'.
    2. Module body: find declarations terminated with ';' that live after the
       closing ')' of the port list.
    """
    signals: Dict[str, Signal] = {}

    # ── Pass 1: port-list declarations ───────────────────────────────────────
    # Find the module header: module name (...);
    mod_paren_m = re.search(r'\bmodule\b[^(]*\(', code, re.DOTALL)
    port_list_end = 0
    if mod_paren_m:
        # Find matching ')' for the module port list
        start = mod_paren_m.end() - 1  # position of '('
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
        port_list_end = i + 1  # position just after ')'

        # Each entry in the port list is comma-separated
        # But entries can contain nested brackets [msb:lsb]
        # Split by commas that are NOT inside brackets
        entries = _split_port_entries(port_list_content)
        for entry in entries:
            entry = entry.strip()
            if not entry:
                continue
            sig = _parse_single_port_decl(entry)
            if sig and sig.name not in signals:
                signals[sig.name] = sig

    # ── Pass 2: body declarations (after port list, terminated with ';') ─────
    body_code = code[port_list_end:]
    # Match declarations: (input|output|inout|wire|reg) [reg] [[msb:lsb]] names ;
    # Use a lookahead to not cross into another keyword
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
    """Split a port list string on commas, respecting bracket nesting."""
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
    """
    Parse a single port declaration string like:
      'input clk', 'input [7:0] a', 'output reg [8:0] result', 'reg [3:0] count'
    Returns a Signal or None.
    """
    entry = entry.strip()
    if not entry:
        return None

    # Check for a direction keyword
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

    # Plain identifier (ANSI port style where direction declared separately)
    id_m = re.match(r'^([A-Za-z_][A-Za-z0-9_$]*)', entry)
    if id_m:
        name = id_m.group(1)
        if name not in _KEYWORDS:
            return Signal(name=name, kind='wire', width=1)

    return None


def parse(verilog: str) -> ParsedModule:
    """
    Parse a Verilog module from source text.
    Returns a ParsedModule with signals, processes, and any parse errors.
    """
    errors: List[str] = []
    code = _strip_comments(verilog)

    # Extract module name
    mod_m = re.search(r'\bmodule\s+(\w+)', code)
    if not mod_m:
        return ParsedModule(
            name='unknown',
            signals={},
            processes=[],
            errors=['No module declaration found']
        )
    module_name = mod_m.group(1)

    # Parse signals
    signals = _parse_signals(code)

    processes: List[Process] = []
    proc_counter = {'assign': 0, 'always': 0}

    # --- Parse assign statements ---
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
        # Filter reads to only known signals plus new ones we may find
        drives = [lhs_name] if lhs_name not in _KEYWORDS else []

        processes.append(Process(
            name=proc_name,
            kind='combinational',
            sensitivity=reads,
            drives=drives,
            reads=reads,
        ))
        # Register lhs as wire if not already known
        if lhs_name not in signals and lhs_name not in _KEYWORDS:
            signals[lhs_name] = Signal(name=lhs_name, kind='wire', width=1)

    # --- Parse always blocks ---
    always_blocks = _find_always_blocks(code)
    for _, (sens_str, body) in enumerate(always_blocks):
        proc_counter['always'] += 1
        kind, sensitivity = _parse_sensitivity_list(sens_str)

        # Extract all identifiers from body
        all_idents = _extract_identifiers(body)

        # Extract driven signals (LHS of assignments)
        drives = _extract_lhs_names(body)
        # Deduplicate while preserving order
        seen_drives = set()
        unique_drives = []
        for d in drives:
            if d not in seen_drives:
                seen_drives.add(d)
                unique_drives.append(d)
        drives = unique_drives

        # Reads = all identifiers that are not exclusively on LHS and not keywords
        reads = [i for i in all_idents if i not in seen_drives or i in sensitivity]
        # If sensitivity is '*', reads become the sensitivity list
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

    return ParsedModule(
        name=module_name,
        signals=signals,
        processes=processes,
        errors=errors,
    )
