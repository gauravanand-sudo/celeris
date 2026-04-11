"""
analyzer.py — Hot-path analysis and optimization suggestions for Celeris RTL Simulator.

Computes signal/process heat metrics, traces the critical path, and generates
actionable optimization suggestions based on simulation results.
"""

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Set
from rtl_parser import ParsedModule, Process, Signal
from simulator import SimResults


@dataclass
class HotSignal:
    name: str
    kind: str
    width: int
    activations: int
    heat: float   # 0.0 to 1.0 relative to max


@dataclass
class HotProcess:
    name: str
    kind: str
    activations: int
    heat: float
    drives: List[str]


@dataclass
class Suggestion:
    severity: str    # 'error', 'warning', 'info'
    category: str
    message: str
    signal: Optional[str] = None


@dataclass
class Analysis:
    module_name: str
    total_signals: int
    total_processes: int
    sim_cycles: int
    total_activations: int
    hot_signals: List[HotSignal]      # sorted descending by activations
    hot_processes: List[HotProcess]   # sorted descending by activations
    critical_path: List[str]          # signal names forming hottest chain
    suggestions: List[Suggestion]


def analyze(module: ParsedModule, results: SimResults) -> Analysis:
    """
    Analyze simulation results and produce hot-path metrics and suggestions.
    """
    sig_acts = results.signal_activations
    proc_acts = results.process_activations

    total_activations = sum(sig_acts.values()) + sum(proc_acts.values())

    # --- Compute hot signals ---
    max_sig_act = max(sig_acts.values(), default=1)
    if max_sig_act == 0:
        max_sig_act = 1

    hot_signals: List[HotSignal] = []
    for name, sig in module.signals.items():
        acts = sig_acts.get(name, 0)
        heat = acts / max_sig_act
        hot_signals.append(HotSignal(
            name=name,
            kind=sig.kind,
            width=sig.width,
            activations=acts,
            heat=heat,
        ))
    # Also include signals that appeared in activations but not in module.signals
    for name, acts in sig_acts.items():
        if name not in module.signals:
            heat = acts / max_sig_act
            hot_signals.append(HotSignal(
                name=name,
                kind='wire',
                width=1,
                activations=acts,
                heat=heat,
            ))
    hot_signals.sort(key=lambda s: s.activations, reverse=True)

    # --- Compute hot processes ---
    max_proc_act = max(proc_acts.values(), default=1)
    if max_proc_act == 0:
        max_proc_act = 1

    hot_processes: List[HotProcess] = []
    proc_by_name: Dict[str, Process] = {p.name: p for p in module.processes}
    for name, acts in proc_acts.items():
        heat = acts / max_proc_act
        proc = proc_by_name.get(name)
        kind = proc.kind if proc else 'combinational'
        drives = proc.drives if proc else []
        hot_processes.append(HotProcess(
            name=name,
            kind=kind,
            activations=acts,
            heat=heat,
            drives=drives,
        ))
    hot_processes.sort(key=lambda p: p.activations, reverse=True)

    # --- Build connectivity maps ---
    # signal -> list of processes that drive it (via proc.drives)
    # signal -> list of processes sensitive to it
    sig_to_driving_procs: Dict[str, List[Process]] = {}
    sig_to_sensitive_procs: Dict[str, List[Process]] = {}

    for proc in module.processes:
        for driven in proc.drives:
            sig_to_driving_procs.setdefault(driven, []).append(proc)
        for read_sig in proc.sensitivity:
            sig_to_sensitive_procs.setdefault(read_sig, []).append(proc)

    # fanout: signal -> number of processes sensitive to it
    fanout: Dict[str, int] = {}
    for sig_name in module.signals:
        fanout[sig_name] = len(sig_to_sensitive_procs.get(sig_name, []))

    # --- Critical path ---
    # Start from hottest signal, greedily follow: sig -> proc (drives from sig) -> next sig (driven by proc)
    critical_path = _build_critical_path(hot_signals, sig_to_sensitive_procs, proc_by_name, sig_acts, max_sig_act)

    # --- Generate suggestions ---
    suggestions = _generate_suggestions(
        module, results, hot_signals, hot_processes,
        sig_to_driving_procs, sig_to_sensitive_procs, fanout,
        max_sig_act
    )

    return Analysis(
        module_name=module.name,
        total_signals=len(module.signals),
        total_processes=len(module.processes),
        sim_cycles=results.cycles,
        total_activations=total_activations,
        hot_signals=hot_signals,
        hot_processes=hot_processes,
        critical_path=critical_path,
        suggestions=suggestions,
    )


def _build_critical_path(
    hot_signals: List[HotSignal],
    sig_to_sensitive_procs: Dict[str, List[Process]],
    proc_by_name: Dict[str, Process],
    sig_acts: Dict[str, int],
    max_sig_act: int,
) -> List[str]:
    """
    Build a critical path of 3-8 signal names by greedily following the
    hottest drives->reads->drives chain starting from the hottest signal.
    """
    if not hot_signals:
        return []

    visited_sigs: Set[str] = set()
    path: List[str] = []

    # Start from hottest signal
    current = hot_signals[0].name
    path.append(current)
    visited_sigs.add(current)

    for _ in range(7):  # max 7 more steps = 8 total
        # Find processes sensitive to current signal
        sensitive = sig_to_sensitive_procs.get(current, [])
        if not sensitive:
            break

        # Pick the most active sensitive process
        best_proc = max(
            sensitive,
            key=lambda p: sum(sig_acts.get(d, 0) for d in p.drives),
            default=None
        )
        if best_proc is None or not best_proc.drives:
            break

        # Pick the hottest driven signal
        best_next = max(
            best_proc.drives,
            key=lambda d: sig_acts.get(d, 0)
        )

        if best_next in visited_sigs or sig_acts.get(best_next, 0) == 0:
            break

        path.append(best_next)
        visited_sigs.add(best_next)
        current = best_next

    # If path is too short, fill with next hottest signals
    if len(path) < 3:
        for hs in hot_signals:
            if hs.name not in visited_sigs and hs.activations > 0:
                path.append(hs.name)
                visited_sigs.add(hs.name)
                if len(path) >= 3:
                    break

    return path[:8]


def _generate_suggestions(
    module: ParsedModule,
    results: SimResults,
    hot_signals: List[HotSignal],
    hot_processes: List[HotProcess],
    sig_to_driving_procs: Dict[str, List[Process]],
    sig_to_sensitive_procs: Dict[str, List[Process]],
    fanout: Dict[str, int],
    max_sig_act: int,
) -> List[Suggestion]:
    """Generate optimization suggestions based on analysis."""
    suggestions: List[Suggestion] = []
    seen_messages: Set[str] = set()

    def add_suggestion(s: Suggestion):
        key = (s.category, s.message)
        if key not in seen_messages:
            seen_messages.add(key)
            suggestions.append(s)

    proc_acts = results.process_activations

    # 1. Combinational Loop: process reads and drives the same signal
    for proc in module.processes:
        if proc.kind == 'combinational':
            overlap = set(proc.reads) & set(proc.drives)
            for sig_name in overlap:
                add_suggestion(Suggestion(
                    severity='error',
                    category='Combinational Loop',
                    message=f"Process '{proc.name}' both reads and drives '{sig_name}' — combinational feedback loop detected.",
                    signal=sig_name,
                ))

    # 2. High Fanout: hot signal drives more than 3 processes
    for hs in hot_signals:
        if hs.heat > 0.7:
            driven_proc_count = len(sig_to_sensitive_procs.get(hs.name, []))
            if driven_proc_count > 3:
                add_suggestion(Suggestion(
                    severity='warning',
                    category='High Fanout',
                    message=(
                        f"Signal '{hs.name}' (heat={hs.heat:.2f}) drives {driven_proc_count} processes. "
                        f"Consider inserting a pipeline register to reduce fanout."
                    ),
                    signal=hs.name,
                ))

    # 3. Missing Clock Gate: sequential process fires every cycle with no enable in reads
    for hp in hot_processes:
        if hp.kind == 'sequential' and hp.activations >= results.cycles:
            proc = next((p for p in module.processes if p.name == hp.name), None)
            if proc:
                has_enable = any(
                    r.lower() in ('en', 'enable', 'ce', 'clk_en', 'valid', 'ready', 'stall')
                    or 'en' in r.lower() or 'enable' in r.lower()
                    for r in proc.reads
                )
                if not has_enable:
                    add_suggestion(Suggestion(
                        severity='warning',
                        category='Missing Clock Gate',
                        message=(
                            f"Sequential process '{hp.name}' fires every cycle with no enable/gate signal in its reads. "
                            f"Add an enable condition to gate the clock and reduce dynamic power."
                        ),
                        signal=None,
                    ))

    # 4. Hot Assign Chain: more than 3 assign (combinational) processes all hot
    hot_assigns = [
        hp for hp in hot_processes
        if hp.kind == 'combinational' and hp.name.startswith('assign_') and hp.heat > 0.5
    ]
    if len(hot_assigns) > 3:
        add_suggestion(Suggestion(
            severity='info',
            category='Hot Assign Chain',
            message=(
                f"Found {len(hot_assigns)} hot combinational assign processes. "
                f"Consider registering intermediate values to break the combinational chain."
            ),
        ))

    # 5. Wide Hot Signal: hot signal with width > 8
    for hs in hot_signals:
        if hs.heat > 0.5 and hs.width > 8:
            add_suggestion(Suggestion(
                severity='info',
                category='Wide Hot Signal',
                message=(
                    f"Signal '{hs.name}' is {hs.width} bits wide and is on the critical path (heat={hs.heat:.2f}). "
                    f"Consider narrowing the bus or pipelining the computation."
                ),
                signal=hs.name,
            ))

    # 6. Pipeline Opportunity: module has both sequential and hot combinational assigns
    has_sequential = any(p.kind == 'sequential' for p in module.processes)
    has_hot_comb = any(
        hp for hp in hot_processes
        if hp.kind == 'combinational' and hp.heat > 0.4
    )
    if has_sequential and has_hot_comb:
        add_suggestion(Suggestion(
            severity='info',
            category='Pipeline Opportunity',
            message=(
                "Module contains both sequential pipeline stages and hot combinational assign chains. "
                "Two pipeline stages detected — validate inter-stage timing and consider adding pipeline retiming."
            ),
        ))

    return suggestions
