#!/usr/bin/env python3
"""
Learn reference drafting signatures from ZW3D drawings and verify generated output.

Usage:
    python ZW3D_Agent/reference_learning.py learn --samples TZ-QT-006779 TZ-TP-000633 TZ-ZJ-011571
    python ZW3D_Agent/reference_learning.py verify --samples TZ-QT-006779 TZ-TP-000633 TZ-ZJ-011571 --fresh-zw3d
"""

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from ZW3D_Agent.http_client import ZW3DHttpClient


DEFAULT_SAMPLES = ["TZ-QT-006779", "TZ-TP-000633", "TZ-ZJ-011571"]
DEFAULT_ZW3D_EXE = r"C:\Program Files\ZWSOFT\ZW3D 2026\zw3d.exe"
REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SAMPLE_DIR = REPO_ROOT / "AI-ZW260403"
DEFAULT_PROFILES = Path(__file__).resolve().parent / "reference_profiles.json"
DEFAULT_REPORT_JSON = Path(__file__).resolve().parent / "reference_verify_report.json"
DEFAULT_REPORT_MD = Path(__file__).resolve().parent / "reference_verify_report.md"


def kill_zw3d() -> None:
    subprocess.run(
        ["taskkill", "/IM", "zw3d.exe", "/F"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    time.sleep(2)


def wait_for_server(client: ZW3DHttpClient, max_wait: int = 120) -> bool:
    deadline = time.time() + max_wait
    while time.time() < deadline:
        if client.check_server():
            return True
        time.sleep(3)
    return False


def start_zw3d(path: Optional[Path], zw3d_exe: str) -> subprocess.Popen:
    args = [zw3d_exe]
    if path is not None:
        args.append(str(path))
    return subprocess.Popen(args)


def ensure_server(client: ZW3DHttpClient, seed_path: Path, zw3d_exe: str) -> bool:
    if client.check_server():
        return False
    start_zw3d(seed_path, zw3d_exe)
    if not wait_for_server(client):
        raise RuntimeError("ZW3D HTTP server did not become ready")
    return True


def open_file(client: ZW3DHttpClient, path: Path) -> Dict[str, Any]:
    result = client.open_part(str(path))
    if result.get("status") != "ok":
        raise RuntimeError(f"Failed to open {path}: {result}")
    return result


def wait_for_analysis(client: ZW3DHttpClient, max_wait: int = 90) -> Dict[str, Any]:
    deadline = time.time() + max_wait
    last_result: Dict[str, Any] = {}
    while time.time() < deadline:
        try:
            last_result = client.analyze_part()
            if last_result.get("status") == "ok":
                return last_result
        except Exception as exc:
            last_result = {"status": "error", "message": str(exc)}
        time.sleep(3)
    return last_result or {"status": "error", "message": "Timed out waiting for part analysis"}


def layout_views_from_quality(quality: Dict[str, Any]) -> List[Dict[str, Any]]:
    views = []
    for view in quality.get("views", []):
        view_type = view.get("type")
        if view_type == "definition":
            continue
        center = view.get("alignment_center") or view.get("center") or {}
        views.append(
            {
                "type": view_type,
                "x": round(float(center.get("x", 0.0)), 1),
                "y": round(float(center.get("y", 0.0)), 1),
                "dimensions": int(view.get("dimensions", 0)),
            }
        )
    return views


def profile_to_flat_payload(profile: Dict[str, Any]) -> Dict[str, Any]:
    payload: Dict[str, Any] = {
        "sample": profile["sample"],
        "paper": profile["paper"],
        "template_path": profile.get("drawing_path", ""),
        "base_views": profile["counts"]["base_views"],
        "project_views": profile["counts"]["project_views"],
        "section_views": profile["counts"]["section_views"],
        "view_count": len(profile["views"]),
        "total_dimensions": profile["counts"]["total_dimensions"],
        "annotated_views": profile["counts"]["annotated_views"],
    }
    for index, view in enumerate(profile["views"]):
        payload[f"view{index}_type"] = view["type"]
        payload[f"view{index}_x"] = view["x"]
        payload[f"view{index}_y"] = view["y"]
        payload[f"view{index}_dims"] = view["dimensions"]
    return payload


def learn_one(
    client: ZW3DHttpClient,
    sample: str,
    sample_dir: Path,
    zw3d_exe: str,
    drawing_already_active: bool = False,
) -> Dict[str, Any]:
    drawing_path = sample_dir / f"{sample}.Z3DRW"
    part_path = sample_dir / f"{sample}.Z3PRT"
    if not drawing_path.exists():
        raise FileNotFoundError(drawing_path)
    if not part_path.exists():
        raise FileNotFoundError(part_path)

    if not drawing_already_active:
        try:
            open_file(client, drawing_path)
        except Exception:
            kill_zw3d()
            start_zw3d(drawing_path, zw3d_exe)
            if not wait_for_server(client):
                raise RuntimeError(f"ZW3D server not ready after restarting for {drawing_path}")
    client.set_reference_profile({"view_count": 0})
    quality = client.evaluate_quality()
    inspect = client.inspect_views()
    if quality.get("status") == "error":
        raise RuntimeError(f"Quality read failed for {sample}: {quality}")

    summary = quality.get("summary", {})
    paper = (quality.get("paper") or {}).get("paper_name") or (inspect.get("paper") or {}).get("paper_name")
    views = layout_views_from_quality(quality)

    try:
        open_file(client, part_path)
    except Exception:
        kill_zw3d()
        start_zw3d(part_path, zw3d_exe)
        if not wait_for_server(client):
            raise RuntimeError(f"ZW3D server not ready after restarting for {part_path}")
    analysis = client.analyze_part()
    if analysis.get("status") != "ok":
        raise RuntimeError(f"Part analysis failed for {sample}: {analysis}")

    return {
        "sample": sample,
        "drawing_path": str(drawing_path),
        "part_path": str(part_path),
        "paper": paper,
        "bbox": analysis.get("bbox", {}),
        "profile": analysis.get("recommended_profile", ""),
        "base_view": analysis.get("recommended_base_view", ""),
        "counts": {
            "base_views": int(summary.get("base_views", inspect.get("base_views", 0))),
            "project_views": int(summary.get("project_views", inspect.get("project_views", 0))),
            "section_views": int(summary.get("section_views", inspect.get("section_views", 0))),
            "definition_views": int(summary.get("definition_views", inspect.get("definition_views", 0))),
            "total_dimensions": int(summary.get("total_dimensions", 0)),
            "annotated_views": int(summary.get("annotated_views", 0)),
        },
        "views": views,
    }


def learn_profiles(args: argparse.Namespace) -> int:
    sample_dir = Path(args.sample_dir)
    client = ZW3DHttpClient()
    first_drawing = sample_dir / f"{args.samples[0]}.Z3DRW"
    started_with_first_drawing = ensure_server(client, first_drawing, args.zw3d_exe)

    profiles = {
        "version": 1,
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "sample_dir": str(sample_dir),
        "samples": {},
    }

    for sample in args.samples:
        if args.fresh_zw3d:
            kill_zw3d()
            start_zw3d(sample_dir / f"{sample}.Z3DRW", args.zw3d_exe)
            if not wait_for_server(client):
                raise RuntimeError(f"ZW3D server not ready for {sample}")
        profile = learn_one(
            client,
            sample,
            sample_dir,
            args.zw3d_exe,
            drawing_already_active=(started_with_first_drawing and sample == args.samples[0]),
        )
        profiles["samples"][sample] = profile
        print(f"[learn] {sample}: paper={profile['paper']} dims={profile['counts']['total_dimensions']} views={len(profile['views'])}")

    output_path = Path(args.profiles)
    output_path.write_text(json.dumps(profiles, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"[learn] wrote {output_path}")
    return 0


def load_profiles(path: Path) -> Dict[str, Any]:
    if not path.exists():
        raise FileNotFoundError(path)
    return json.loads(path.read_text(encoding="utf-8"))


def verify_one(client: ZW3DHttpClient, profile: Dict[str, Any], part_already_active: bool = False) -> Dict[str, Any]:
    if not part_already_active:
        open_file(client, Path(profile["part_path"]))
    analysis = wait_for_analysis(client)
    set_result = client.set_reference_profile(profile_to_flat_payload(profile))
    generation = client.generate_smart_drafting()
    pmi = client.add_pmi()
    quality = client.evaluate_quality()
    inspect = client.inspect_views()

    reference_like = (quality.get("metrics") or {}).get("reference_like") or {}
    return {
        "sample": profile["sample"],
        "analysis": analysis,
        "set_reference_profile": set_result,
        "generation_status": generation.get("status"),
        "pmi": pmi,
        "quality": {
            "status": quality.get("status"),
            "passed": quality.get("passed"),
            "score": quality.get("score"),
            "next_action": quality.get("next_action"),
            "reference_like": reference_like,
            "failures": [rule.get("rule") for rule in quality.get("rules", []) if not rule.get("passed")],
        },
        "inspect_counts": {
            "base_views": inspect.get("base_views"),
            "project_views": inspect.get("project_views"),
            "section_views": inspect.get("section_views"),
            "definition_views": inspect.get("definition_views"),
        },
    }


def write_markdown_report(results: Iterable[Dict[str, Any]], path: Path) -> None:
    lines = [
        "# ZW3D Reference Verification",
        "",
        "| Sample | Reference-like | Passed | Accepted | Score | Next action | Failures |",
        "|---|---:|---:|---:|---:|---|---|",
    ]
    for result in results:
        quality = result["quality"]
        ref_like = quality.get("reference_like", {}).get("matched")
        accepted = quality.get("passed") and quality.get("next_action") == "accept"
        failures = ", ".join(quality.get("failures", [])) or "-"
        lines.append(
            f"| {result['sample']} | {ref_like} | {quality.get('passed')} | {accepted} | "
            f"{quality.get('score')} | {quality.get('next_action')} | {failures} |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def verify_profiles(args: argparse.Namespace) -> int:
    all_profiles = load_profiles(Path(args.profiles))
    sample_dir = Path(args.sample_dir)
    client = ZW3DHttpClient()
    first_part = sample_dir / f"{args.samples[0]}.Z3PRT"
    if not args.fresh_zw3d:
        ensure_server(client, first_part, args.zw3d_exe)

    results = []
    for sample in args.samples:
        profile = all_profiles["samples"][sample]
        if args.fresh_zw3d:
            kill_zw3d()
            start_zw3d(None, args.zw3d_exe)
            if not wait_for_server(client):
                raise RuntimeError(f"ZW3D server not ready for {sample}")
        result = verify_one(client, profile, part_already_active=False)
        results.append(result)
        quality = result["quality"]
        print(
            f"[verify] {sample}: reference_like={quality['reference_like'].get('matched')} "
            f"passed={quality['passed']} score={quality['score']} next={quality['next_action']}"
        )

    report_json = Path(args.report_json)
    report_md = Path(args.report_md)
    report_json.write_text(json.dumps({"results": results}, ensure_ascii=False, indent=2), encoding="utf-8")
    write_markdown_report(results, report_md)
    print(f"[verify] wrote {report_json}")
    print(f"[verify] wrote {report_md}")

    failed = [
        result for result in results
        if not result["quality"].get("passed")
        or result["quality"].get("next_action") != "accept"
    ]
    return 1 if failed else 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Learn and verify ZW3D reference drafting profiles")
    parser.add_argument("--sample-dir", default=str(DEFAULT_SAMPLE_DIR), help="Directory containing reference Z3DRW/Z3PRT files")
    parser.add_argument("--profiles", default=str(DEFAULT_PROFILES), help="Path to reference_profiles.json")
    parser.add_argument("--zw3d-exe", default=DEFAULT_ZW3D_EXE, help="Path to zw3d.exe")
    subparsers = parser.add_subparsers(dest="command", required=True)

    learn = subparsers.add_parser("learn", help="Learn profiles from reference drawings")
    learn.add_argument("--samples", nargs="+", default=DEFAULT_SAMPLES)
    learn.add_argument("--fresh-zw3d", action="store_true", help="Restart ZW3D for each sample while learning")
    learn.set_defaults(func=learn_profiles)

    verify = subparsers.add_parser("verify", help="Verify generated drawings against learned profiles")
    verify.add_argument("--samples", nargs="+", default=DEFAULT_SAMPLES)
    verify.add_argument("--fresh-zw3d", action="store_true", help="Restart ZW3D for each sample while verifying")
    verify.add_argument("--report-json", default=str(DEFAULT_REPORT_JSON))
    verify.add_argument("--report-md", default=str(DEFAULT_REPORT_MD))
    verify.set_defaults(func=verify_profiles)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
