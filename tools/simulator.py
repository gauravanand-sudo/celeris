"""
simulator.py — Structural simulation engine for Celeris RTL Simulator.

Does not evaluate logic. Propagates signal activations through the
connectivity graph to produce activation counts used for hot-path analysis.
"""

from dataclasses import dataclass, field
from typing import Dict, List, Set
from rtl_parser import ParsedModule, Process


@dataclass
class SimResults:
    cycles: int
    signal_activations: Dict[str, int]
    process_activations: Dict[str, int]
    delta_cycles_total: int


def simulate(module: ParsedModule, num_cycles: int = 500) -> SimResults:
    """
    Structurally simulate a parsed module for num_cycles clock cycles.

    Algorithm:
    - Build a sensitivity map: signal_name -> list of processes sensitive to it.
    - Each cycle: toggle clock signals, fire sensitive processes, propagate
      activations through driven signals recursively (depth-limited to 20).
    - First 5 cycles: also activate reset signals.
    - Every 10 cycles: toggle other input signals as stimulus.
    - Track signal_activations and process_activations counts.
    """

    signal_activations: Dict[str, int] = {name: 0 for name in module.signals}
    process_activations: Dict[str, int] = {p.name: 0 for p in module.processes}
    delta_cycles_total = 0

    # Build sensitivity map: signal -> processes sensitive to it
    sens_map: Dict[str, List[Process]] = {}
    for proc in module.processes:
        for sig_name in proc.sensitivity:
            if sig_name not in sens_map:
                sens_map[sig_name] = []
            sens_map[sig_name].append(proc)

    # Categorize signals
    clock_signals: Set[str] = set()
    reset_signals: Set[str] = set()
    other_inputs: Set[str] = set()

    for name, sig in module.signals.items():
        lower = name.lower()
        if lower in ('clk', 'clock') or lower.startswith('clk_') or lower.endswith('_clk'):
            clock_signals.add(name)
        elif lower in ('rst', 'reset', 'rst_n', 'reset_n') or lower.startswith('rst'):
            reset_signals.add(name)
        elif sig.kind == 'input':
            other_inputs.add(name)

    # If no clock found, treat any input as potential driver
    if not clock_signals:
        for name, sig in module.signals.items():
            if sig.kind == 'input' and name not in reset_signals:
                clock_signals.add(name)

    def fire_process(proc: Process, visited_procs: Set[str], depth: int, delta_count: List[int]):
        """Fire a process: record activation, propagate to driven signals."""
        if depth > 20:
            return
        if proc.name in visited_procs:
            return
        visited_procs.add(proc.name)
        process_activations[proc.name] = process_activations.get(proc.name, 0) + 1
        delta_count[0] += 1

        for driven_sig in proc.drives:
            activate_signal(driven_sig, visited_procs, depth + 1, delta_count)

    def activate_signal(sig_name: str, visited_procs: Set[str], depth: int, delta_count: List[int]):
        """Activate a signal: record activation, fire sensitive processes."""
        if depth > 20:
            return
        if sig_name in signal_activations:
            signal_activations[sig_name] += 1
        else:
            signal_activations[sig_name] = 1

        for proc in sens_map.get(sig_name, []):
            fire_process(proc, visited_procs, depth + 1, delta_count)

    for cycle in range(num_cycles):
        visited_procs: Set[str] = set()
        delta_count = [0]

        # Toggle clock signals every cycle
        for clk_name in clock_signals:
            activate_signal(clk_name, visited_procs, 0, delta_count)

        # Activate reset signals in first 5 cycles
        if cycle < 5:
            for rst_name in reset_signals:
                activate_signal(rst_name, visited_procs, 0, delta_count)

        # Toggle other input signals every 10 cycles as stimulus
        if cycle % 10 == 0:
            for inp_name in other_inputs:
                activate_signal(inp_name, visited_procs, 0, delta_count)

        delta_cycles_total += delta_count[0]

    return SimResults(
        cycles=num_cycles,
        signal_activations=signal_activations,
        process_activations=process_activations,
        delta_cycles_total=delta_cycles_total,
    )
