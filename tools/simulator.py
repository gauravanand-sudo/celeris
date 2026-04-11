"""
simulator.py — Structural simulation engine for Celeris RTL Simulator.
"""

from dataclasses import dataclass, field
from typing import Dict, List, Set
from rtl_parser import ParsedModule, Process
import re


@dataclass
class RegionStats:
    name: str           # Active, NBA, Postponed
    activations: int
    description: str


@dataclass
class SimResults:
    cycles: int
    signal_activations: Dict[str, int]
    process_activations: Dict[str, int]
    delta_cycles_total: int
    max_queue_depth: int
    avg_delta_per_cycle: float
    region_stats: List[RegionStats]
    clock_signals: List[str]
    reset_signals: List[str]
    event_log: List[str]        # last N notable simulation events


def simulate(module: ParsedModule, num_cycles: int = 500) -> SimResults:
    signal_activations: Dict[str, int] = {name: 0 for name in module.signals}
    process_activations: Dict[str, int] = {p.name: 0 for p in module.processes}
    delta_cycles_total = 0
    max_queue_depth = 0
    event_log: List[str] = []

    # Region counters (IEEE 1800-2017 scheduling regions)
    region_active = 0      # combinational + sequential process evaluations
    region_nba = 0         # non-blocking assignment updates (<=)
    region_postponed = 0   # $monitor-style reads (output signals sampled)

    # Build sensitivity map
    sens_map: Dict[str, List[Process]] = {}
    for proc in module.processes:
        for sig_name in proc.sensitivity:
            sens_map.setdefault(sig_name, []).append(proc)

    # Categorize signals
    clock_sigs: Set[str] = set()
    reset_sigs: Set[str] = set()
    other_inputs: Set[str] = set()

    for name, sig in module.signals.items():
        lower = name.lower()
        if lower in ('clk', 'clock') or lower.startswith('clk_') or lower.endswith('_clk'):
            clock_sigs.add(name)
        elif re.search(r'\brst\b|\breset\b|^rst_|_rst$|^rst$', lower):
            reset_sigs.add(name)
        elif sig.kind == 'input':
            other_inputs.add(name)

    if not clock_sigs:
        for name, sig in module.signals.items():
            if sig.kind == 'input' and name not in reset_sigs:
                clock_sigs.add(name)

    def fire_process(proc: Process, visited: Set[str], depth: int, queue: List[int]):
        if depth > 20 or proc.name in visited:
            return
        visited.add(proc.name)
        process_activations[proc.name] = process_activations.get(proc.name, 0) + 1
        queue[0] += 1

        nonlocal region_active, region_nba, region_postponed
        if proc.kind == 'sequential':
            region_nba += 1          # sequential uses non-blocking <=
        else:
            region_active += 1       # combinational resolves in Active region

        for driven in proc.drives:
            activate_signal(driven, visited, depth + 1, queue)

    def activate_signal(sig: str, visited: Set[str], depth: int, queue: List[int]):
        if depth > 20:
            return
        signal_activations[sig] = signal_activations.get(sig, 0) + 1
        queue[0] += 1

        nonlocal region_postponed
        # Output signals sampled at end of time step (Postponed region)
        if sig in module.signals and module.signals[sig].kind in ('output',):
            region_postponed += 1

        for proc in sens_map.get(sig, []):
            fire_process(proc, visited, depth + 1, queue)

    for cycle in range(num_cycles):
        visited: Set[str] = set()
        queue = [0]

        for clk in clock_sigs:
            activate_signal(clk, visited, 0, queue)

        if cycle < 5:
            for rst in reset_sigs:
                activate_signal(rst, visited, 0, queue)

        if cycle % 10 == 0:
            for inp in other_inputs:
                activate_signal(inp, visited, 0, queue)

        depth = queue[0]
        delta_cycles_total += depth
        if depth > max_queue_depth:
            max_queue_depth = depth

        # Log notable cycles
        if cycle == 0:
            event_log.append(f"[T=0] Reset + clock edge — {depth} events queued, {len(visited)} processes fired")
        elif cycle == 5:
            event_log.append(f"[T=5] Reset released — normal operation begins")
        elif cycle == 10:
            event_log.append(f"[T=10] Stimulus inputs toggled — {depth} propagation events")
        elif cycle == num_cycles // 2:
            event_log.append(f"[T={cycle}] Mid-sim — {depth} events this cycle")
        elif cycle == num_cycles - 1:
            event_log.append(f"[T={cycle}] Final cycle — {depth} events, simulation complete")

    avg_delta = delta_cycles_total / num_cycles if num_cycles > 0 else 0

    region_stats = [
        RegionStats("Active",    region_active,    "Combinational logic evaluation + blocking assignments"),
        RegionStats("NBA",       region_nba,       "Non-blocking assignment (<=) updates to registers"),
        RegionStats("Postponed", region_postponed, "Output signal sampling ($monitor / $strobe)"),
    ]

    return SimResults(
        cycles=num_cycles,
        signal_activations=signal_activations,
        process_activations=process_activations,
        delta_cycles_total=delta_cycles_total,
        max_queue_depth=max_queue_depth,
        avg_delta_per_cycle=round(avg_delta, 2),
        region_stats=region_stats,
        clock_signals=sorted(clock_sigs),
        reset_signals=sorted(reset_sigs),
        event_log=event_log,
    )
