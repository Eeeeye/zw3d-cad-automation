#!/usr/bin/env python3
"""
ZW3D Drafting Agent Test Scenarios

Tests the agent against reference sample parts from AI-ZW260403
and synthetic shapes.

Usage:
    python ZW3D_Agent/test_scenarios.py              # run all
    python ZW3D_Agent/test_scenarios.py --scenario 1  # run specific scenario
    python ZW3D_Agent/test_scenarios.py --wait        # wait for ZW3D to start
"""

import argparse
import json
import logging
import sys
import os
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ZW3D_Agent.http_client import ZW3DHttpClient
from ZW3D_Agent.zw3d_agent import ZW3DAgent, AgentConfig
from ZW3D_Agent.strategies import SAMPLES

logger = logging.getLogger("zw3d_agent_test")

# Base path for sample files
SAMPLE_BASE = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "AI-ZW260403")


def setup_logging():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )


def scenario_1_simple_block(client: ZW3DHttpClient) -> dict:
    """Scenario 1: Create a simple block 80x60x40 and run agent."""
    print("\n" + "=" * 60)
    print("  Scenario 1: Simple Block (80x60x40)")
    print("=" * 60)

    # Create block first
    result = client.create_block(80, 60, 40)
    if result.get("status") != "ok":
        return {"passed": False, "error": f"Block creation failed: {result}"}

    time.sleep(1)

    # Run agent
    config = AgentConfig(max_iterations=5, pass_score=85.0)
    agent = ZW3DAgent(client=client, config=config)
    return agent.run()


def scenario_2_plate_thin(client: ZW3DHttpClient) -> dict:
    """Scenario 2: TZ-TP-000633 - Standard plate-like part (A3 reference)."""
    print("\n" + "=" * 60)
    print("  Scenario 2: TZ-TP-000633 (plate_like, A3 reference)")
    print("=" * 60)

    part_path = os.path.join(SAMPLE_BASE, "TZ-TP-000633.Z3PRT")
    if not os.path.exists(part_path):
        return {"status": "skipped", "error": f"Sample not found: {part_path}"}

    config = AgentConfig(max_iterations=5, pass_score=85.0)
    agent = ZW3DAgent(client=client, config=config)
    return agent.run(part_path=part_path)


def scenario_3_small_plate(client: ZW3DHttpClient) -> dict:
    """Scenario 3: TZ-QT-006779 - Small plate-like part (A4 reference)."""
    print("\n" + "=" * 60)
    print("  Scenario 3: TZ-QT-006779 (plate_like, A4 reference)")
    print("=" * 60)

    part_path = os.path.join(SAMPLE_BASE, "TZ-QT-006779.Z3PRT")
    if not os.path.exists(part_path):
        return {"status": "skipped", "error": f"Sample not found: {part_path}"}

    config = AgentConfig(max_iterations=5, pass_score=85.0)
    agent = ZW3DAgent(client=client, config=config)
    return agent.run(part_path=part_path)


def scenario_4_stepped(client: ZW3DHttpClient) -> dict:
    """Scenario 4: TZ-ZJ-011571 - Stepped part (A3 reference, 2 base views)."""
    print("\n" + "=" * 60)
    print("  Scenario 4: TZ-ZJ-011571 (stepped, A3 reference)")
    print("=" * 60)

    part_path = os.path.join(SAMPLE_BASE, "TZ-ZJ-011571.Z3PRT")
    if not os.path.exists(part_path):
        return {"status": "skipped", "error": f"Sample not found: {part_path}"}

    config = AgentConfig(max_iterations=5, pass_score=80.0)
    agent = ZW3DAgent(client=client, config=config)
    return agent.run(part_path=part_path)


def scenario_5_large_shell(client: ZW3DHttpClient) -> dict:
    """Scenario 5: TZ-GJ-006199 - Large shell/housing (A1 reference, sections needed)."""
    print("\n" + "=" * 60)
    print("  Scenario 5: TZ-GJ-006199 (large shell, A1 reference)")
    print("=" * 60)

    part_path = os.path.join(SAMPLE_BASE, "TZ-GJ-006199-显示器后壳.Z3PRT")
    if not os.path.exists(part_path):
        return {"status": "skipped", "error": f"Sample not found: {part_path}"}

    config = AgentConfig(max_iterations=5, pass_score=75.0)
    agent = ZW3DAgent(client=client, config=config)
    return agent.run(part_path=part_path)


def scenario_6_cover_with_base2(client: ZW3DHttpClient) -> dict:
    """Scenario 6: TZ-GJ-006769 - Cover with second base view (A1 reference)."""
    print("\n" + "=" * 60)
    print("  Scenario 6: TZ-GJ-006769 (cover, A1 reference)")
    print("=" * 60)

    part_path = os.path.join(SAMPLE_BASE, "TZ-GJ-006769-下盖.Z3PRT")
    if not os.path.exists(part_path):
        return {"status": "skipped", "error": f"Sample not found: {part_path}"}

    config = AgentConfig(max_iterations=5, pass_score=75.0)
    agent = ZW3DAgent(client=client, config=config)
    return agent.run(part_path=part_path)


SCENARIOS = {
    1: ("Simple Block", scenario_1_simple_block),
    2: ("Thin Plate TZ-TP-000633", scenario_2_plate_thin),
    3: ("Small Plate TZ-QT-006779", scenario_3_small_plate),
    4: ("Stepped TZ-ZJ-011571", scenario_4_stepped),
    5: ("Large Shell TZ-GJ-006199", scenario_5_large_shell),
    6: ("Cover TZ-GJ-006769", scenario_6_cover_with_base2),
}


def print_result(scenario_id: int, name: str, result: dict):
    """Print a formatted test result."""
    from dataclasses import asdict
    r = asdict(result) if hasattr(result, '__dataclass_fields__') else result
    status = r.get("status", "unknown")
    score = r.get("score", 0)
    iters = r.get("iterations", 0)
    profile = r.get("profile", "")
    base_view = r.get("base_view", "")

    icon = "PASS" if status in ("passed", "accepted_with_warnings") else "FAIL"
    if status == "skipped":
        icon = "SKIP"

    print(f"\n  [{icon}] Scenario {scenario_id}: {name}")
    print(f"    Status: {status} | Score: {score:.1f} | Iterations: {iters}")
    print(f"    Profile: {profile} | Base view: {base_view}")


def main():
    parser = argparse.ArgumentParser(description="ZW3D Drafting Agent Test Scenarios")
    parser.add_argument("--scenario", type=int, default=None,
                       help="Run specific scenario (1-6)")
    parser.add_argument("--wait", action="store_true",
                       help="Wait for ZW3D server to start")
    args = parser.parse_args()

    setup_logging()

    client = ZW3DHttpClient()

    # Check server
    if args.wait:
        if not client.wait_for_server(max_wait=120):
            print("ERROR: ZW3D HTTP Server not available")
            sys.exit(1)
    elif not client.check_server():
        print("ERROR: ZW3D HTTP Server not running")
        sys.exit(1)

    results = {}
    scenarios_to_run = {args.scenario: SCENARIOS[args.scenario]} if args.scenario else SCENARIOS

    for sid, (name, func) in scenarios_to_run.items():
        try:
            result = func(client)
            results[sid] = result
            print_result(sid, name, result)
        except Exception as e:
            logger.error("Scenario %d failed with exception: %s", sid, e)
            results[sid] = {"status": "error", "error": str(e), "score": 0}
            print_result(sid, name, results[sid])

    # Summary
    print("\n" + "=" * 60)
    print("  Summary")
    print("=" * 60)
    from dataclasses import asdict
    def get_status(r):
        if hasattr(r, 'status'):
            return r.status
        return r.get("status", "unknown")
    passed = sum(1 for r in results.values()
                 if get_status(r) in ("passed", "accepted_with_warnings"))
    failed = sum(1 for r in results.values()
                 if get_status(r) not in ("passed", "accepted_with_warnings", "skipped"))
    skipped = sum(1 for r in results.values()
                  if get_status(r) == "skipped")
    print(f"  Passed: {passed} | Failed: {failed} | Skipped: {skipped}")
    print("=" * 60)

    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
