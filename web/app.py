"""
app.py — FastAPI backend for Celeris RTL Analyzer web application.
"""

from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import sys
import os

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


@app.post("/api/simulate")
def run_simulation(req: SimRequest):
    try:
        module = parse(req.verilog)
        results = simulate(module, req.cycles)
        analysis = analyze(module, results)

        return {
            "module_name": analysis.module_name,
            "total_signals": analysis.total_signals,
            "total_processes": analysis.total_processes,
            "sim_cycles": analysis.sim_cycles,
            "total_activations": analysis.total_activations,
            "hot_signals": [vars(s) for s in analysis.hot_signals],
            "hot_processes": [vars(p) for p in analysis.hot_processes],
            "critical_path": analysis.critical_path,
            "suggestions": [vars(s) for s in analysis.suggestions],
            "parse_errors": module.errors,
        }
    except Exception as e:
        raise HTTPException(status_code=400, detail=str(e))


app.mount(
    "/",
    StaticFiles(
        directory=os.path.join(os.path.dirname(__file__), "static"),
        html=True
    ),
    name="static",
)
