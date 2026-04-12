"""
app.py — FastAPI backend for Celeris RTL Analyzer.
"""

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import sys, os, dataclasses, subprocess, json

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

# Path to the compiled C++ binary
_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_BIN  = os.path.join(_ROOT, 'build', 'celeris')

def _try_compile():
    """Try to compile the C++ binary at startup if it doesn't exist."""
    if os.path.isfile(_BIN):
        return
    try:
        os.makedirs(os.path.join(_ROOT, 'build'), exist_ok=True)
        r = subprocess.run(
            ['g++', '-std=c++20', '-O3', '-pthread', '-Iinclude',
             '-o', _BIN, 'src/main.cpp'],
            cwd=_ROOT, capture_output=True, text=True, timeout=120
        )
        if r.returncode == 0:
            print(f'[startup] compiled celeris binary at {_BIN}', flush=True)
        else:
            print(f'[startup] compile failed: {r.stderr[:300]}', flush=True)
    except Exception as e:
        print(f'[startup] compile error: {e}', flush=True)

_try_compile()


# ── Benchmark endpoint (real C++ engine) ─────────────────────────────────────

class BenchmarkRequest(BaseModel):
    num_threads: int = 3
    num_events: int = 200


@app.post("/api/benchmark")
def run_benchmark(req: BenchmarkRequest):
    num_threads = max(1, min(req.num_threads, 8))
    num_events  = max(10, min(req.num_events, 2000))

    if not os.path.isfile(_BIN):
        raise HTTPException(status_code=503, detail={
            "error": "C++ binary not found. See README to compile: g++ -std=c++20 -O3 -pthread -Iinclude -o build/celeris src/main.cpp"
        })

    try:
        result = subprocess.run(
            [_BIN, '--json', '--threads', str(num_threads), '--events', str(num_events)],
            capture_output=True, text=True, timeout=60
        )
    except subprocess.TimeoutExpired:
        raise HTTPException(status_code=504, detail={"error": "Benchmark timed out (>60s)"})
    except Exception as e:
        raise HTTPException(status_code=500, detail={"error": str(e)})

    if result.returncode != 0:
        raise HTTPException(status_code=500, detail={
            "error": f"Binary exited with code {result.returncode}",
            "stderr": result.stderr[:500]
        })

    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as e:
        raise HTTPException(status_code=500, detail={
            "error": f"Failed to parse engine output: {e}",
            "raw": result.stdout[:500]
        })


# ── RTL structural analysis endpoint ─────────────────────────────────────────

class SimRequest(BaseModel):
    verilog: str
    cycles: int = 500


def _validate(module, verilog: str) -> list[str]:
    errors = list(module.errors)
    if 'endmodule' not in verilog and not any('endmodule' in e for e in errors):
        errors.append("Missing 'endmodule' — module is not closed")
    if not module.signals and not any('signal' in e for e in errors):
        errors.append("No signals found — check port declarations (input/output/wire/reg)")
    if not module.processes and not any('process' in e for e in errors):
        errors.append("No processes found — no always blocks or assign statements detected")
    return errors


@app.post("/api/simulate")
def run_simulation(req: SimRequest):
    if not req.verilog.strip():
        raise HTTPException(status_code=400, detail={
            "stage": "parse",
            "errors": ["Empty Verilog input"],
        })

    try:
        module = parse(req.verilog)
        critical_errors = _validate(module, req.verilog)
        if critical_errors:
            raise HTTPException(status_code=422, detail={
                "stage": "parse",
                "errors": critical_errors,
                "warnings": module.warnings,
            })

        cycles = max(1, min(req.cycles, 100_000))
        results = simulate(module, cycles)
        analysis = analyze(module, results)

        return {
            "module_name":         analysis.module_name,
            "total_signals":       analysis.total_signals,
            "total_processes":     analysis.total_processes,
            "sim_cycles":          analysis.sim_cycles,
            "total_activations":   analysis.total_activations,
            "delta_cycles":        results.delta_cycles_total,
            "max_queue_depth":     results.max_queue_depth,
            "avg_delta_per_cycle": results.avg_delta_per_cycle,
            "clock_signals":       results.clock_signals,
            "reset_signals":       results.reset_signals,
            "region_stats":        [dataclasses.asdict(r) for r in results.region_stats],
            "event_log":           results.event_log,
            "hot_signals":         [dataclasses.asdict(s) for s in analysis.hot_signals],
            "hot_processes":       [dataclasses.asdict(p) for p in analysis.hot_processes],
            "critical_path":       analysis.critical_path,
            "suggestions":         [dataclasses.asdict(s) for s in analysis.suggestions],
            "warnings":            module.warnings,
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
