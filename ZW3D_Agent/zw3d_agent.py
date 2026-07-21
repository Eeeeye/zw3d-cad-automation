"""
ZW3D Drafting Agent - Iterative analysis-generation-evaluation-correction loop.

The agent calls ZW3D HTTP endpoints in a loop:
  1. Analyze part → select strategy
  2. Generate drafting
  3. Evaluate quality
  4. Apply corrective action based on quality feedback
  5. Repeat until quality passes or max iterations reached
"""

import logging
import json
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Dict, Any, List, Optional

from .http_client import ZW3DHttpClient, ZW3DHTTPError
from .strategies import get_strategy, get_corrective_actions, PartTypeStrategy

logger = logging.getLogger("zw3d_agent")


@dataclass
class AgentConfig:
    """Agent configuration."""
    max_iterations: int = 5
    pass_score: float = 85.0
    pmi_retry_limit: int = 2        # max consecutive PMI retries before accepting
    open_part_wait: float = 2.0     # seconds to wait after opening a part


@dataclass
class IterationRecord:
    """Record of a single agent iteration."""
    iteration: int
    action: str = ""
    generation_status: str = ""
    quality_score: float = 0.0
    quality_passed: bool = False
    next_action: str = ""
    error_rules: List[str] = field(default_factory=list)
    warning_rules: List[str] = field(default_factory=list)
    details: Dict[str, Any] = field(default_factory=dict)


@dataclass
class AgentResult:
    """Final result of the agent run."""
    status: str = ""                # "passed", "accepted_with_warnings", "max_iterations", "error"
    score: float = 0.0
    iterations: int = 0
    profile: str = ""
    base_view: str = ""
    history: List[Dict[str, Any]] = field(default_factory=list)
    error: str = ""


class ZW3DAgent:
    """Iterative drafting agent for ZW3D part annotation."""

    def __init__(self, client: ZW3DHttpClient, config: AgentConfig = None):
        self.client = client
        self.config = config or AgentConfig()
        self._pmi_retry_count = 0
        self._last_pmi_action = ""

    @staticmethod
    def _profile_to_flat_payload(profile: Dict[str, Any]) -> Dict[str, Any]:
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

    @staticmethod
    def _load_reference_profile_for_part(part_path: Optional[str]) -> Optional[Dict[str, Any]]:
        if not part_path:
            return None
        profiles_path = Path(__file__).with_name("reference_profiles.json")
        if not profiles_path.exists():
            return None
        try:
            profiles = json.loads(profiles_path.read_text(encoding="utf-8")).get("samples", {})
        except (OSError, json.JSONDecodeError):
            return None

        stem = Path(part_path).stem
        for sample, profile in profiles.items():
            if stem == sample or stem.startswith(sample):
                return profile
        return None

    def run(self, part_path: Optional[str] = None) -> AgentResult:
        """
        Main agent loop.

        Args:
            part_path: Path to .Z3PRT file. If None, uses currently active part.

        Returns:
            AgentResult with final status, score, and iteration history.
        """
        history: List[IterationRecord] = []
        self._pmi_retry_count = 0
        self._last_pmi_action = ""

        # Phase 0: Setup
        if not self.client.check_server():
            return AgentResult(status="error", error="ZW3D HTTP Server not running")

        if part_path:
            logger.info("Opening part: %s", part_path)
            try:
                open_result = self.client.open_part(part_path)
                if open_result.get("status") != "ok":
                    return AgentResult(status="error",
                                       error=f"Failed to open part: {open_result.get('message', '')}")
                time.sleep(self.config.open_part_wait)
            except ZW3DHTTPError as e:
                return AgentResult(status="error", error=f"Open part request failed: {e}")

        # Phase 1: Analyze
        logger.info("Analyzing active part...")
        try:
            analysis = self.client.analyze_part()
        except ZW3DHTTPError as e:
            return AgentResult(status="error", error=f"Part analysis failed: {e}")

        if analysis.get("status") != "ok":
            return AgentResult(status="error",
                               error=f"Part analysis returned error: {analysis.get('message', '')}")

        profile = analysis.get("recommended_profile", "blocky")
        base_view = analysis.get("recommended_base_view", "front")
        strategy = get_strategy(profile)

        reference_profile = self._load_reference_profile_for_part(part_path)
        if reference_profile:
            logger.info("Injecting learned reference profile: %s", reference_profile.get("sample"))
            try:
                self.client.set_reference_profile(self._profile_to_flat_payload(reference_profile))
            except ZW3DHTTPError as e:
                return AgentResult(status="error", error=f"Set reference profile failed: {e}")
        elif part_path:
            logger.info("No learned reference profile matched; clearing active reference profile")
            try:
                self.client.set_reference_profile({"view_count": 0})
            except ZW3DHTTPError as e:
                logger.warning("Failed to clear reference profile: %s", e)

        logger.info("Part profile=%s, base_view=%s", profile, base_view)
        logger.info("Strategy: papers=%s, sections=%s, pass_score=%.0f",
                     strategy.paper_sequence, strategy.needs_section_views, strategy.pass_score)

        # Use strategy's pass score if user didn't override
        effective_pass_score = self.config.pass_score
        if effective_pass_score >= 85.0:  # user used default
            effective_pass_score = strategy.pass_score

        # Phase 2: Agent loop
        last_quality = None
        last_score = -1.0
        stuck_count = 0  # count consecutive iterations with no score improvement

        for iteration in range(self.config.max_iterations):
            record = IterationRecord(iteration=iteration)
            logger.info("--- Iteration %d/%d ---", iteration + 1, self.config.max_iterations)

            # Step A: Generate or apply correction
            if iteration == 0:
                gen_result = self._do_generate_smart()
                record.action = "generate_smart_drafting"
            else:
                gen_result = self._apply_correction(last_quality, strategy)
                record.action = gen_result.get("_action", "unknown")

            record.generation_status = gen_result.get("status", "unknown")
            record.details["generation"] = gen_result

            # Step B: Evaluate quality
            try:
                quality = self.client.evaluate_quality()
            except ZW3DHTTPError as e:
                logger.error("Quality evaluation failed: %s", e)
                record.details["quality_error"] = str(e)
                history.append(record)
                last_quality = None  # Signal that quality is unavailable
                continue

            score = quality.get("score", 0.0)
            next_action = quality.get("next_action", "accept")
            passed = quality.get("passed", False)
            error_rules = [r.get("rule", "") for r in quality.get("rules", [])
                          if not r.get("passed", True) and r.get("severity") == "error"]
            warning_rules = [r.get("rule", "") for r in quality.get("rules", [])
                            if not r.get("passed", True) and r.get("severity") == "warning"]

            if next_action == "add_pmi" and "pmi_present" in error_rules:
                logger.info("Quality evaluator requested PMI; adding dimensions immediately")
                try:
                    pmi_result = self.client.add_pmi()
                    quality = self.client.evaluate_quality()
                    score = quality.get("score", 0.0)
                    next_action = quality.get("next_action", "accept")
                    passed = quality.get("passed", False)
                    error_rules = [r.get("rule", "") for r in quality.get("rules", [])
                                  if not r.get("passed", True) and r.get("severity") == "error"]
                    warning_rules = [r.get("rule", "") for r in quality.get("rules", [])
                                    if not r.get("passed", True) and r.get("severity") == "warning"]
                    record.details["pmi"] = pmi_result
                except ZW3DHTTPError as e:
                    logger.warning("Immediate PMI failed: %s", e)
                    record.details["pmi_error"] = str(e)

            record.quality_score = score
            record.quality_passed = passed
            record.next_action = next_action
            record.error_rules = error_rules
            record.warning_rules = warning_rules
            record.details["quality"] = quality
            history.append(record)

            logger.info("Score=%.1f, passed=%s, next_action=%s", score, passed, next_action)
            if error_rules:
                logger.info("  Failed rules: %s", ", ".join(error_rules))
            if warning_rules:
                logger.info("  Warnings: %s", ", ".join(warning_rules))

            # Step C: Check termination
            if next_action == "accept":
                logger.info("Quality evaluator accepts the drawing")
                return AgentResult(
                    status="passed" if score >= effective_pass_score else "accepted_with_warnings",
                    score=score, iterations=iteration + 1,
                    profile=profile, base_view=base_view,
                    history=[asdict(r) for r in history])

            if passed and score >= effective_pass_score:
                logger.info("Quality passed with non-blocking warnings at iteration %d (score=%.1f >= %.1f, next_action=%s)",
                           iteration + 1, score, effective_pass_score, next_action)
                return AgentResult(
                    status="accepted_with_warnings", score=score, iterations=iteration + 1,
                    profile=profile, base_view=base_view,
                    history=[asdict(r) for r in history])

            if iteration == self.config.max_iterations - 1:
                logger.warning("Max iterations reached (score=%.1f)", score)
                return AgentResult(
                    status="max_iterations", score=score, iterations=iteration + 1,
                    profile=profile, base_view=base_view,
                    history=[asdict(r) for r in history])

            # Step D: Detect stuck loop and try PMI if missing
            if score <= last_score + 1.0:
                stuck_count += 1
            else:
                stuck_count = 0
            last_score = score

            if stuck_count >= 2 and "pmi_present" in error_rules:
                # Stuck on layout issues + PMI missing: add PMI and re-evaluate
                # Do NOT clear views, as that would undo the PMI we just added
                logger.info("Score stuck at %.1f for %d iterations, trying PMI...", score, stuck_count)
                try:
                    pmi_result = self.client.add_pmi()
                    logger.info("PMI result: %s", pmi_result.get("status"))
                    # Re-evaluate immediately
                    quality2 = self.client.evaluate_quality()
                    score2 = quality2.get("score", 0.0)
                    next_action2 = quality2.get("next_action", "accept")
                    logger.info("After PMI: score=%.1f, next_action=%s", score2, next_action2)
                    if score2 >= effective_pass_score or next_action2 == "accept":
                        return AgentResult(
                            status="passed" if score2 >= effective_pass_score else "accepted_with_warnings",
                            score=score2, iterations=iteration + 1,
                            profile=profile, base_view=base_view,
                            history=[asdict(r) for r in history])
                    # PMI helped but not enough — stop clearing views to preserve PMI
                    # Override next_action so the correction step won't clear+regenerate
                    if score2 > score:
                        logger.info("PMI improved score from %.1f to %.1f, accepting with warnings", score, score2)
                        return AgentResult(
                            status="accepted_with_warnings",
                            score=score2, iterations=iteration + 1,
                            profile=profile, base_view=base_view,
                            history=[asdict(r) for r in history])
                    # Use the new quality for next correction
                    quality = quality2
                    last_quality = quality2
                    last_score = score2
                    continue
                except ZW3DHTTPError as e:
                    logger.warning("PMI attempt failed: %s", e)

            # Step E: Save quality for next iteration's correction
            last_quality = quality

        return AgentResult(
            status="exhausted", score=0, iterations=self.config.max_iterations,
            profile=profile, base_view=base_view,
            history=[asdict(r) for r in history])

    def _do_generate_smart(self) -> Dict[str, Any]:
        """Run smart drafting generation."""
        logger.info("Running generate_smart_drafting...")
        try:
            return self.client.generate_smart_drafting()
        except ZW3DHTTPError as e:
            return {"status": "error", "message": str(e)}

    def _apply_correction(self, quality: Dict[str, Any],
                          strategy: PartTypeStrategy) -> Dict[str, Any]:
        """Apply corrective action based on quality evaluation feedback."""
        if quality is None:
            # Quality evaluation failed — regenerate from scratch
            logger.warning("No quality feedback available, regenerating from scratch")
            next_action = "generate_smart_drafting"
        else:
            next_action = quality.get("next_action", "generate_smart_drafting")
        actions = get_corrective_actions(next_action)

        logger.info("Applying correction for next_action='%s': %d step(s)",
                    next_action, len(actions))

        combined_result = {"_action": next_action}
        for action_name, params in actions:
            logger.info("  Step: %s %s", action_name, params or "")

            try:
                if action_name == "generate_smart_drafting":
                    result = self.client.generate_smart_drafting()
                    self._pmi_retry_count = 0

                elif action_name == "clear_sheet_views":
                    result = self.client.clear_sheet_views()

                elif action_name == "regenerate_drafting":
                    paper_offset = params.get("paper_index_offset", 0)
                    scale_mult = params.get("scale_multiplier", 1.0)
                    result = self.client.regenerate_drafting(paper_offset, scale_mult)
                    self._pmi_retry_count = 0

                elif action_name == "add_pmi":
                    # Track consecutive PMI retries to avoid infinite loops
                    if self._last_pmi_action == "add_pmi":
                        self._pmi_retry_count += 1
                    else:
                        self._pmi_retry_count = 1

                    if self._pmi_retry_count > strategy.max_pmi_retries:
                        logger.warning("PMI retry limit reached (%d), skipping",
                                      self._pmi_retry_count)
                        combined_result["_pmi_skipped"] = True
                        continue

                    result = self.client.add_pmi()

                elif action_name == "add_section_view":
                    label = params.get("label", "A")
                    position = params.get("position", "below")
                    result = self.client.add_section_view(label, position)

                else:
                    logger.warning("Unknown corrective action: %s", action_name)
                    continue

                combined_result[action_name] = result
                self._last_pmi_action = action_name

            except ZW3DHTTPError as e:
                logger.error("  Action %s failed: %s", action_name, e)
                combined_result[f"{action_name}_error"] = str(e)

        return combined_result
