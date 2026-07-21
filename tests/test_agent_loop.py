from ZW3D_Agent.zw3d_agent import AgentConfig, ZW3DAgent


class FakeClient:
    def __init__(self, quality_results):
        self.calls = []
        self.quality_results = list(quality_results)

    def check_server(self):
        self.calls.append("check_server")
        return True

    def analyze_part(self):
        self.calls.append("analyze_part")
        return {
            "status": "ok",
            "recommended_profile": "plate_like",
            "recommended_base_view": "top",
        }

    def generate_smart_drafting(self):
        self.calls.append("generate_smart_drafting")
        return {"status": "ok"}

    def evaluate_quality(self):
        self.calls.append("evaluate_quality")
        if self.quality_results:
            return self.quality_results.pop(0)
        return {"status": "ok", "passed": True, "score": 90, "next_action": "accept", "rules": []}

    def add_pmi(self):
        self.calls.append("add_pmi")
        return {"status": "ok"}

    def clear_sheet_views(self):
        self.calls.append("clear_sheet_views")
        return {"status": "ok"}

    def regenerate_drafting(self, paper_index_offset=0, scale_multiplier=1.0):
        self.calls.append(
            ("regenerate_drafting", paper_index_offset, scale_multiplier)
        )
        return {"status": "ok"}

    def add_section_view(self, label="A", position="below"):
        self.calls.append(("add_section_view", label, position))
        return {"status": "ok"}


def test_agent_adds_pmi_when_quality_requires_it():
    client = FakeClient(
        [
            {
                "status": "ok",
                "passed": False,
                "score": 55,
                "next_action": "add_pmi",
                "rules": [
                    {"rule": "pmi_present", "passed": False, "severity": "error"}
                ],
            },
            {
                "status": "ok",
                "passed": True,
                "score": 91,
                "next_action": "accept",
                "rules": [],
            },
        ]
    )

    result = ZW3DAgent(client, AgentConfig(max_iterations=3)).run()

    assert result.status == "passed"
    assert result.score == 91
    assert "add_pmi" in client.calls


def test_agent_applies_regeneration_action_on_next_iteration():
    client = FakeClient(
        [
            {
                "status": "ok",
                "passed": False,
                "score": 42,
                "next_action": "regenerate_with_smaller_scale_and_larger_spacing",
                "rules": [
                    {"rule": "views_fit_paper", "passed": False, "severity": "error"}
                ],
            },
            {
                "status": "ok",
                "passed": True,
                "score": 87,
                "next_action": "accept",
                "rules": [],
            },
        ]
    )

    result = ZW3DAgent(client, AgentConfig(max_iterations=3)).run()

    assert result.status == "passed"
    assert ("regenerate_drafting", 1, 0.88) in client.calls
    assert "clear_sheet_views" in client.calls
