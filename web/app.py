"""
app.py — FastAPI backend for Celeris RTL Analyzer.
"""

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import sys, os, dataclasses

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from rtl_parser import parse
from simulator import simulate
from analyzer import analyze

app = FastAPI(title="Celeris RTL Analyzer")

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["POST", "GET"],
    allow_headers=["*"],
)


class SimRequest(BaseModel):
    verilog: str
    cycles: int = 500


def _validate(module, verilog: str) -> list[str]:
    """Return critical parse errors that should block simulation."""
    errors = list(module.errors)

    if 'endmodule' not in verilog:
        errors.append("Missing 'endmodule' — module is not closed")

    if not module.signals:
        errors.append("No signals found — check port declarations (input/output/wire/reg)")

    if not module.processes:
        errors.append("No processes found — no always blocks or assign statements detected")

    return errors


@app.post("/api/simulate")
def run_simulation(req: SimRequest):
    if not req.verilog.strip():
        raise HTTPException(status_code=400, detail="Empty Verilog input")

    try:
        # ── Step 1: Parse ──────────────────────────────────────────
        module = parse(req.verilog)

        critical_errors = _validate(module, req.verilog)
        if critical_errors:
            raise HTTPException(status_code=422, detail={
                "stage": "parse",
                "errors": critical_errors,
            })

        # ── Step 2: Simulate ───────────────────────────────────────
        cycles = max(1, min(req.cycles, 100_000))
        results = simulate(module, cycles)

        # ── Step 3: Analyze ────────────────────────────────────────
        analysis = analyze(module, results)

        return {
            "module_name":       analysis.module_name,
            "total_signals":     analysis.total_signals,
            "total_processes":   analysis.total_processes,
            "sim_cycles":        analysis.sim_cycles,
            "total_activations": analysis.total_activations,
            "delta_cycles":      results.delta_cycles_total,
            "max_queue_depth":   results.max_queue_depth,
            "avg_delta_per_cycle": results.avg_delta_per_cycle,
            "clock_signals":     results.clock_signals,
            "reset_signals":     results.reset_signals,
            "region_stats":      [dataclasses.asdict(r) for r in results.region_stats],
            "event_log":         results.event_log,
            "hot_signals":       [dataclasses.asdict(s) for s in analysis.hot_signals],
            "hot_processes":     [dataclasses.asdict(p) for p in analysis.hot_processes],
            "critical_path":     analysis.critical_path,
            "suggestions":       [dataclasses.asdict(s) for s in analysis.suggestions],
        }

    except HTTPException:
        raise
    except Exception as e:
        raise HTTPException(status_code=400, detail={"stage": "runtime", "errors": [str(e)]})


app.mount(
    "/",
    StaticFiles(directory=os.path.join(os.path.dirname(__file__), "static"), html=True),
    name="static",
)
