#!/usr/bin/env python3
"""
ZW3D Drafting Agent CLI

Usage:
    python ZW3D_Agent/run_agent.py --active
    python ZW3D_Agent/run_agent.py --part "C:\\path\\to\\part.Z3PRT"
    python ZW3D_Agent/run_agent.py --part "AI-ZW260403/TZ-TP-000633.Z3PRT" --max-iterations 5
"""

import argparse
import json
import logging
import sys
import os

# Add parent directory to path for imports
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ZW3D_Agent.http_client import ZW3DHttpClient
from ZW3D_Agent.zw3d_agent import ZW3DAgent, AgentConfig


def setup_logging(verbose: bool = False):
    """Configure logging for the agent."""
    level = logging.DEBUG if verbose else logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
        datefmt="%H:%M:%S",
    )


def main():
    parser = argparse.ArgumentParser(description="ZW3D Drafting Agent")
    parser.add_argument("--part", type=str, default=None,
                       help="Path to .Z3PRT file (uses active part if omitted)")
    parser.add_argument("--active", action="store_true",
                       help="Use the currently active part in ZW3D")
    parser.add_argument("--max-iterations", type=int, default=5,
                       help="Maximum agent loop iterations (default: 5)")
    parser.add_argument("--pass-score", type=float, default=85.0,
                       help="Quality score threshold to pass (default: 85, auto-adjusted per profile)")
    parser.add_argument("--verbose", "-v", action="store_true",
                       help="Enable verbose/debug logging")
    parser.add_argument("--wait", action="store_true",
                       help="Wait for ZW3D server to start (up to 120s)")
    args = parser.parse_args()

    setup_logging(args.verbose)

    client = ZW3DHttpClient()

    # Check server
    if args.wait:
        print("Waiting for ZW3D HTTP Server...")
        if not client.wait_for_server(max_wait=120):
            print("ERROR: ZW3D HTTP Server not available after 120s")
            sys.exit(1)
    elif not client.check_server():
        print("ERROR: ZW3D HTTP Server not running. Start ZW3D with the plugin loaded.")
        print("  Use --wait to automatically wait for the server.")
        sys.exit(1)

    # Resolve part path
    part_path = args.part
    if part_path and not os.path.isabs(part_path):
        part_path = os.path.abspath(part_path)

    if part_path and not os.path.exists(part_path):
        print(f"ERROR: Part file not found: {part_path}")
        sys.exit(1)

    # Configure and run agent
    config = AgentConfig(
        max_iterations=args.max_iterations,
        pass_score=args.pass_score,
    )

    agent = ZW3DAgent(client=client, config=config)

    print(f"\n{'='*60}")
    print(f"  ZW3D Drafting Agent")
    print(f"  Part: {part_path or '(active)'}")
    print(f"  Max iterations: {config.max_iterations}")
    print(f"  Pass score: {config.pass_score}")
    print(f"{'='*60}\n")

    result = agent.run(part_path=part_path if not args.active else None)

    # Print results
    print(f"\n{'='*60}")
    print(f"  Agent Result")
    print(f"{'='*60}")
    print(f"  Status:     {result.status}")
    print(f"  Score:      {result.score:.1f}")
    print(f"  Iterations: {result.iterations}")
    print(f"  Profile:    {result.profile}")
    print(f"  Base view:  {result.base_view}")

    if result.error:
        print(f"  Error:      {result.error}")

    # Print iteration history summary
    if result.history:
        print(f"\n  Iteration History:")
        for h in result.history:
            it = h.get("iteration", "?")
            score = h.get("quality_score", 0)
            action = h.get("action", "")
            next_act = h.get("next_action", "")
            errors = h.get("error_rules", [])
            print(f"    [{it}] score={score:.1f} action={action} next={next_act}")
            if errors:
                print(f"         errors: {', '.join(errors)}")

    print(f"{'='*60}\n")

    # Exit code
    if result.status in ("passed", "accepted_with_warnings"):
        sys.exit(0)
    else:
        sys.exit(1)


if __name__ == "__main__":
    main()
