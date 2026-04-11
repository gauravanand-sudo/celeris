#!/usr/bin/env python3
"""
Celeris RTL Simulator & Hot Path Analyzer — Command-Line Interface

Usage: python tools/cli.py design.v [--cycles N]
"""

import sys
import os
import argparse

sys.path.insert(0, os.path.dirname(__file__))

from rtl_parser import parse
from simulator import simulate
from analyzer import analyze


def main():
    parser = argparse.ArgumentParser(
        description='Celeris RTL Simulator & Hot Path Analyzer',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            'Examples:\n'
            '  python tools/cli.py design.v\n'
            '  python tools/cli.py design.v --cycles 1000\n'
        )
    )
    parser.add_argument('file', help='Verilog source file')
    parser.add_argument('--cycles', type=int, default=500, help='Simulation cycles (default: 500)')
    args = parser.parse_args()

    with open(args.file) as f:
        verilog = f.read()

    module = parse(verilog)
    if module.errors:
        for e in module.errors:
            print(f"Parse error: {e}")

    results = simulate(module, args.cycles)
    analysis = analyze(module, results)

    # Print formatted report
    print(f"\n{'='*60}")
    print(f"  Celeris Analysis: {analysis.module_name}")
    print(f"{'='*60}")
    print(f"  Signals: {analysis.total_signals}  Processes: {analysis.total_processes}")
    print(f"  Cycles: {analysis.sim_cycles}  Total activations: {analysis.total_activations}")

    print(f"\n  HOT SIGNALS (top 10):")
    for s in analysis.hot_signals[:10]:
        bar = '\u2588' * int(s.heat * 30)
        print(f"  {s.name:<20} {bar:<30} {s.activations:>6}")

    print(f"\n  HOT PROCESSES (top 10):")
    for p in analysis.hot_processes[:10]:
        bar = '\u2588' * int(p.heat * 30)
        print(f"  {p.name:<20} {bar:<30} {p.activations:>6}")

    if analysis.critical_path:
        print(f"\n  CRITICAL PATH:")
        print(f"  {' \u2192 '.join(analysis.critical_path)}")

    print(f"\n  SUGGESTIONS:")
    icons = {'error': '\u2716', 'warning': '\u26a0', 'info': '\u2139'}
    for s in analysis.suggestions:
        print(f"  {icons.get(s.severity, '\u2022')} [{s.category}] {s.message}")

    print()


if __name__ == '__main__':
    main()
