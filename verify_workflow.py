#!/usr/bin/env python3
"""
End-to-end verifier for the ZW3D bridge workflow.

It validates:
1. HTTP server availability
2. Part creation
3. Drawing generation
4. Embedded PMI result in /generate_drafting
5. Standalone PMI execution through /add_pmi
"""

import json
import sys
from typing import Any, Dict

import requests


BASE_URL = "http://localhost:8081"


def log(message: str) -> None:
    print(f"[VERIFY] {message}")


def call(method: str, path: str, payload: Dict[str, Any] | None = None, timeout: int = 60) -> Dict[str, Any]:
    url = f"{BASE_URL}{path}"
    response = requests.request(method, url, json=payload, timeout=timeout)
    response.raise_for_status()
    return response.json()


def print_json(title: str, data: Dict[str, Any]) -> None:
    log(title)
    print(json.dumps(data, indent=2, ensure_ascii=False))


def main() -> int:
    try:
        status = call("GET", "/status", timeout=5)
        print_json("Server status:", status)
    except Exception as exc:
        log(f"Server check failed: {exc}")
        log("Make sure ZW3D is running and the DLL has been loaded from apilibs.")
        return 1

    try:
        create_result = call(
            "POST",
            "/create_block",
            {"length": 100, "width": 60, "height": 40},
            timeout=30,
        )
        print_json("Create block result:", create_result)
    except Exception as exc:
        log(f"Block creation failed: {exc}")
        return 1

    try:
        drafting_result = call("POST", "/generate_drafting", {}, timeout=120)
        print_json("Generate drafting result:", drafting_result)
    except Exception as exc:
        log(f"Drafting generation failed: {exc}")
        return 1

    embedded_pmi = drafting_result.get("pmi")
    if isinstance(embedded_pmi, dict):
        print_json("Embedded PMI result:", embedded_pmi)
    else:
        log(f"Embedded PMI result is not structured JSON: {embedded_pmi!r}")

    try:
        pmi_result = call("POST", "/add_pmi", {}, timeout=60)
        print_json("Standalone PMI result:", pmi_result)
    except Exception as exc:
        log(f"Standalone PMI call failed: {exc}")
        return 1

    if drafting_result.get("status") != "ok":
        log("Drafting request did not return ok.")
        return 1

    if pmi_result.get("status") != "ok":
        log("Standalone PMI request did not return ok.")
        return 1

    log("Workflow verification finished.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
