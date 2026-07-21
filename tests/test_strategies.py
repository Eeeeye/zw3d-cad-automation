from ZW3D_Agent.strategies import get_corrective_actions, get_strategy


def test_unknown_profile_uses_blocky_default():
    strategy = get_strategy("unknown-profile")

    assert strategy.profile == "blocky"
    assert strategy.paper_sequence[0] == "A4(V)"


def test_regenerate_quality_action_maps_to_clear_and_regenerate():
    actions = get_corrective_actions("regenerate_with_smaller_scale_and_larger_spacing")

    assert actions == [
        ("clear_sheet_views", {}),
        ("regenerate_drafting", {"paper_index_offset": 1, "scale_multiplier": 0.88}),
    ]


def test_unknown_quality_action_falls_back_to_smart_generation():
    assert get_corrective_actions("unexpected") == [("generate_smart_drafting", {})]
