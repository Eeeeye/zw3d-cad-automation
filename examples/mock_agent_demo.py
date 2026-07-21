#!/usr/bin/env python3
"""Run the drafting agent with a fake ZW3D client.

This demo is intentionally independent of ZW3D. It gives reviewers a quick way
to see the agent loop, quality feedback, and corrective action behavior.
"""

from __future__ import annotations

import json
import sys
from dataclasses import asdict
from pathlib import Path
from typing import Any, Dict, List

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from ZW3D_Agent.zw3d_agent import AgentConfig, ZW3DAgent


class ScriptedZW3DClient:
    """Small fake client that mimics the subset of ZW3D used by the agent."""

    def __init__(self) -> None:
        self.calls: List[str] = []
        self._quality_results = [
            {
                "status": "ok",
                "passed": False,
                "score": 58.0,
                "next_action": "add_pmi",
                "rules": [
                    {
                        "rule": "pmi_present",
                        "passed": False,
                        "severity": "error",
                    }
                ],
            },
            {
                "status": "ok",
                "passed": True,
                "score": 91.0,
                "next_action": "accept",
                "rules": [],
            },
        ]

    def check_server(self) -> bool:
        self.calls.append("check_server")
        return True

    def analyze_part(self) -> Dict[str, Any]:
        self.calls.append("analyze_part")
        return {
            "status": "ok",
            "recommended_profile": "plate_like",
            "recommended_base_view": "top",
            "bbox": {"x": 320.0, "y": 210.0, "z": 16.0},
        }

    def generate_smart_drafting(self) -> Dict[str, Any]:
        self.calls.append("generate_smart_drafting")
        return {
            "status": "ok",
            "paper": "A1(H)",
            "views": ["base", "project_top", "project_right"],
        }

    def evaluate_quality(self) -> Dict[str, Any]:
        self.calls.append("evaluate_quality")
        if self._quality_results:
            return self._quality_results.pop(0)
        return {
            "status": "ok",
            "passed": True,
            "score": 91.0,
            "next_action": "accept",
            "rules": [],
        }

    def add_pmi(self) -> Dict[str, Any]:
        self.calls.append("add_pmi")
        return {"status": "ok", "dimensions_added": 18}

    def clear_sheet_views(self) -> Dict[str, Any]:
        self.calls.append("clear_sheet_views")
        return {"status": "ok"}

    def regenerate_drafting(
        self,
        paper_index_offset: int = 0,
        scale_multiplier: float = 1.0,
    ) -> Dict[str, Any]:
        self.calls.append(
            f"regenerate_drafting(offset={paper_index_offset}, scale={scale_multiplier})"
        )
        return {"status": "ok"}

    def add_section_view(self, label: str = "A", position: str = "below") -> Dict[str, Any]:
        self.calls.append(f"add_section_view(label={label}, position={position})")
        return {"status": "ok"}


def main() -> int:
    client = ScriptedZW3DClient()
    agent = ZW3DAgent(client=client, config=AgentConfig(max_iterations=3))
    result = agent.run()

    print("Agent result:")
    print(json.dumps(asdict(result), indent=2))
    print("\nFake ZW3D API call trace:")
    for index, call in enumerate(client.calls, start=1):
        print(f"{index:02d}. {call}")

    return 0 if result.status in {"passed", "accepted_with_warnings"} else 1


if __name__ == "__main__":
    raise SystemExit(main())
