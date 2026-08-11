import importlib.util
import unittest
from pathlib import Path

import numpy as np

spec = importlib.util.spec_from_file_location("grid_audit", Path(__file__).parents[1] / "grid-audit.py")
grid = importlib.util.module_from_spec(spec); spec.loader.exec_module(grid)

followup_spec = importlib.util.spec_from_file_location("grid_followup", Path(__file__).parents[1] / "grid-followup.py")
followup = importlib.util.module_from_spec(followup_spec); followup_spec.loader.exec_module(followup)


def pattern(values, fpb=20, bars=40):
    env = np.zeros(fpb * len(values) * bars)
    for i in range(len(values) * bars):
        env[i * fpb] = values[i % len(values)]
    # onset envelope相当の短い裾
    return np.convolve(env, np.array([1.0, .4, .1]), mode="same")


class HumanAnswerProtectionTests(unittest.TestCase):
    """曲を足して再監査したときに、前回の人間回答が機械判定で消えないこと。

    実際に消した事故があるので回帰として固定する（正例23曲の human_verified_4_4 が
    否定例10曲を足した再監査で needs_review へ戻った）。
    """

    def test_human_verified_is_protected(self):
        self.assertTrue(grid.human_decided({"grid_status": "human_verified_4_4"}))
        self.assertTrue(grid.human_decided({"grid_status": "human_verified_non_4_4"}))

    def test_human_unknown_with_review_is_protected(self):
        # driftと回答された曲はstatusがunknownでも人の判断。
        self.assertTrue(grid.human_decided({"grid_status": "unknown", "human_review": {"answer": "drift"}}))

    def test_machine_statuses_are_not_protected(self):
        self.assertFalse(grid.human_decided({"grid_status": "auto_verified_4_4"}))
        self.assertFalse(grid.human_decided({"grid_status": "needs_review"}))
        self.assertFalse(grid.human_decided({"grid_status": "unknown"}))
        self.assertFalse(grid.human_decided({}))


class GridTests(unittest.TestCase):
    def test_synthetic_meter_hypotheses(self):
        four = grid.meter_scores(pattern([1, .25, .65, .25]), 20)
        three = grid.meter_scores(pattern([1, .25, .4]), 20)
        six = grid.meter_scores(pattern([1, .2, .2, .7, .2, .2], fpb=10), 20)
        self.assertEqual(max(four, key=four.get), "4/4")
        self.assertEqual(max(three, key=three.get), "3/4")
        self.assertEqual(max(six, key=six.get), "6/8")

    def test_followup_mark_parser(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            readme = Path(tmp) / "README.md"
            readme.write_text("\n".join([
                "## [x] OK (`ok-id`)",
                "## [NG] ZURE (`ng-id`)",
                "## [?] UNKNOWN (`unknown-id`)",
                "## [ ] TODO (`todo-id`)",
            ]))
            self.assertEqual(
                followup.parse_marks(readme),
                {"ok-id": "x", "ng-id": "NG", "unknown-id": "?"},
            )

    def test_followup_answer_ng_promotes_to_variants(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            readme = Path(tmp) / "README.md"
            readme.write_text("\n".join([
                "## [?] NEEDS LOUDER (`again-id`)",
                "- 回答: NG",
                "## [NG] RESOLVED (`resolved-id`)",
                "- 回答: C",
            ]))
            self.assertEqual(followup.review_modes(readme), {"again-id": "NG"})


if __name__ == "__main__": unittest.main()
