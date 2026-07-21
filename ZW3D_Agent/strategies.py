"""
Part-type drafting strategies and corrective action mappings.

Encodes domain knowledge from AI-ZW260403 sample analysis:
  - Paper size selection rules per part profile
  - Section view / second base view requirements
  - Quality rule failure to corrective action mapping
"""

from dataclasses import dataclass, field
from typing import Dict, List, Optional


@dataclass
class PartTypeStrategy:
    """Drafting strategy for a specific part profile."""
    profile: str
    paper_sequence: List[str]       # ordered paper sizes to try
    needs_section_views: bool       # whether section views are common
    needs_second_base_view: bool    # whether a second base view is common
    scale_range: tuple              # (min, max) acceptable scale
    pass_score: float               # minimum quality score to accept
    max_pmi_retries: int            # max PMI cleanup retries before accepting


# Strategy lookup table - keyed by profile name from /analyze_part
STRATEGIES: Dict[str, PartTypeStrategy] = {
    "blocky": PartTypeStrategy(
        profile="blocky",
        paper_sequence=["A4(V)", "A4(H)", "A3(H)"],
        needs_section_views=False,
        needs_second_base_view=False,
        scale_range=(0.35, 0.82),
        pass_score=85.0,
        max_pmi_retries=2,
    ),
    "plate_like": PartTypeStrategy(
        profile="plate_like",
        paper_sequence=["A3(H)", "A2(H)", "A1(H)"],
        needs_section_views=True,   # large plate-like parts need sections
        needs_second_base_view=True, # large plate-like parts may need 2nd base
        scale_range=(0.15, 0.60),
        pass_score=80.0,
        max_pmi_retries=2,
    ),
    "elongated": PartTypeStrategy(
        profile="elongated",
        paper_sequence=["A3(H)", "A2(H)"],
        needs_section_views=False,
        needs_second_base_view=False,
        scale_range=(0.15, 0.50),
        pass_score=80.0,
        max_pmi_retries=2,
    ),
    "stepped": PartTypeStrategy(
        profile="stepped",
        paper_sequence=["A3(H)", "A2(H)", "A1(H)"],
        needs_section_views=True,    # stepped parts often need sections
        needs_second_base_view=True,  # stepped parts often need 2nd base
        scale_range=(0.15, 0.50),
        pass_score=75.0,
        max_pmi_retries=2,
    ),
}

DEFAULT_STRATEGY = STRATEGIES["blocky"]


def get_strategy(profile: str) -> PartTypeStrategy:
    """Get the drafting strategy for a given profile name."""
    return STRATEGIES.get(profile, DEFAULT_STRATEGY)


# ============================================================
# Corrective action mapping
# ============================================================

# Maps quality evaluator's next_action to agent correction steps.
# Each correction is a tuple of (action_name, params).
# The agent loop applies these sequentially.

CORRECTIVE_ACTIONS = {
    "generate_smart_drafting": [
        ("generate_smart_drafting", {}),
    ],
    "regenerate_with_smaller_scale_and_larger_spacing": [
        ("clear_sheet_views", {}),
        ("regenerate_drafting", {"paper_index_offset": 1, "scale_multiplier": 0.88}),
    ],
    "regenerate_orthographic_layout": [
        ("clear_sheet_views", {}),
        ("regenerate_drafting", {"paper_index_offset": 0, "scale_multiplier": 0.92}),
    ],
    "add_pmi": [
        ("add_pmi", {}),
    ],
    "cleanup_pmi_or_reduce_dimension_density": [
        ("add_pmi", {}),  # re-add triggers internal cleanup
    ],
    "add_section_or_secondary_base_views": [
        ("add_section_view", {"label": "A", "position": "below"}),
    ],
    "review_warnings_and_regenerate_if_needed": [
        ("clear_sheet_views", {}),
        ("generate_smart_drafting", {}),
    ],
    "accept": [],
}


def get_corrective_actions(next_action: str) -> List[tuple]:
    """Get the list of corrective action steps for a quality evaluator next_action."""
    return CORRECTIVE_ACTIONS.get(next_action, [("generate_smart_drafting", {})])


# ============================================================
# Sample reference data (from AI-ZW260403-接口提取规范.md)
# ============================================================

@dataclass
class SampleReference:
    """Reference data from a real ZW3D drafting sample."""
    name: str
    bbox_x: float
    bbox_y: float
    bbox_z: float
    expected_profile: str
    expected_paper: str
    base_views: int
    project_views: int
    section_views: int
    definition_views: int


SAMPLES: Dict[str, SampleReference] = {
    "TZ-QT-006779": SampleReference(
        name="TZ-QT-006779",
        bbox_x=19.5, bbox_y=2.0, bbox_z=32.5,
        expected_profile="plate_like",
        expected_paper="A4(V)",
        base_views=1, project_views=3, section_views=0, definition_views=1,
    ),
    "TZ-TP-000633": SampleReference(
        name="TZ-TP-000633",
        bbox_x=88.9, bbox_y=1.02, bbox_z=46.4,
        expected_profile="plate_like",
        expected_paper="A3(H)",
        base_views=1, project_views=3, section_views=0, definition_views=0,
    ),
    "TZ-ZJ-011571": SampleReference(
        name="TZ-ZJ-011571",
        bbox_x=57.56, bbox_y=15.5, bbox_z=31.7,
        expected_profile="stepped",
        expected_paper="A3(H)",
        base_views=2, project_views=3, section_views=0, definition_views=1,
    ),
    "TZ-GJ-006198": SampleReference(
        name="TZ-GJ-006198-显示器前壳",
        bbox_x=0, bbox_y=0, bbox_z=0,  # ASM - analyze_part not reliable
        expected_profile="plate_like",
        expected_paper="A1(H)",
        base_views=1, project_views=4, section_views=2, definition_views=6,
    ),
    "TZ-GJ-006199": SampleReference(
        name="TZ-GJ-006199-显示器后壳",
        bbox_x=320.44, bbox_y=212.12, bbox_z=16.22,
        expected_profile="plate_like",
        expected_paper="A1(H)",
        base_views=1, project_views=5, section_views=4, definition_views=9,
    ),
    "TZ-GJ-006769": SampleReference(
        name="TZ-GJ-006769-下盖",
        bbox_x=255.47, bbox_y=167.02, bbox_z=11.25,
        expected_profile="plate_like",
        expected_paper="A1(H)",
        base_views=2, project_views=5, section_views=3, definition_views=8,
    ),
}
