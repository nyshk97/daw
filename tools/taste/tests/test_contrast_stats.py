import importlib.util
import unittest
from pathlib import Path

import numpy as np

_spec = importlib.util.spec_from_file_location("contrast_mod", Path(__file__).resolve().parents[1] / "contrast.py")
C = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(C)


def _labelled(pos_values, con_values):
    values = {f"p{i}": v for i, v in enumerate(pos_values)}
    values.update({f"c{i}": v for i, v in enumerate(con_values)})
    roles = {k: ("positive" if k.startswith("p") else "contrast") for k in values}
    return values, roles


class StatsTests(unittest.TestCase):
    def test_auc_is_1_when_groups_are_fully_separated(self):
        values = np.array([1.0, 2.0, 3.0, 10.0, 11.0, 12.0])
        mask = np.array([True, True, True, False, False, False])
        self.assertAlmostEqual(C.auc_from_ranks(C._ranks(values), mask), 0.0)
        self.assertAlmostEqual(C.auc_from_ranks(C._ranks(values), ~mask), 1.0)

    def test_auc_is_half_for_symmetrically_interleaved_groups(self):
        values = np.array([1.0, 2.0, 3.0, 4.0])
        mask = np.array([True, False, False, True])
        self.assertAlmostEqual(C.auc_from_ranks(C._ranks(values), mask), 0.5)

    def test_ties_do_not_depend_on_input_order(self):
        a = C.auc_from_ranks(C._ranks(np.array([1.0, 1.0, 1.0, 1.0])), np.array([True, True, False, False]))
        b = C.auc_from_ranks(C._ranks(np.array([1.0, 1.0, 1.0, 1.0])), np.array([False, False, True, True]))
        self.assertAlmostEqual(a, 0.5)
        self.assertAlmostEqual(b, 0.5)

    def test_shuffled_labels_are_not_significant(self):
        """健全性検査。roleに意味が無いデータで有意になったら統計側のバグ。"""
        rng = np.random.default_rng(7)
        values = rng.normal(size=33)
        mask = np.zeros(33, dtype=bool)
        mask[rng.choice(33, 23, replace=False)] = True
        _, p = C.permutation_p_two_sided(values, mask, C._rng())
        self.assertGreater(p, 0.05)

    def test_separated_groups_are_significant(self):
        values = np.concatenate([np.linspace(0, 1, 23), np.linspace(5, 6, 10)])
        mask = np.zeros(33, dtype=bool)
        mask[:23] = True
        auc, p = C.permutation_p_two_sided(values, mask, C._rng())
        self.assertAlmostEqual(auc, 0.0)
        self.assertLess(p, 0.01)

    def test_benjamini_hochberg_is_more_conservative_than_raw_alpha(self):
        pvalues = {"a": 0.001, "b": 0.04, "c": 0.4, "d": 0.6, "e": 0.9}
        passed = C.benjamini_hochberg(pvalues, alpha=0.05)
        self.assertTrue(passed["a"])
        self.assertFalse(passed["b"])  # 生のp<0.05だがBHでは落ちる
        self.assertFalse(passed["c"])

    def test_overlap_is_high_for_same_distribution_and_low_when_disjoint(self):
        rng = np.random.default_rng(3)
        a = rng.normal(size=200)
        b = rng.normal(size=200)
        self.assertGreater(C.overlap_coefficient(a, b), 0.7)
        self.assertLess(C.overlap_coefficient(a, b + 20), 0.05)

    def test_loo_churn_is_zero_for_a_robust_axis(self):
        values, roles = _labelled(np.linspace(0, 1, 23), np.linspace(5, 6, 10))
        self.assertEqual(C._loo_churn(values, roles), 0.0)

    def test_loo_churn_flags_an_axis_carried_by_one_song(self):
        # 9曲ぶんだけでは中立域に収まり、1曲の外れ値でぎりぎり分離側へ出る軸。
        pos = list(np.linspace(0.0, 1.0, 23))
        con = [0.43] * 9 + [-1.0]
        values, roles = _labelled(pos, con)
        self.assertTrue(C._separates(C._plain_auc(values, roles)))
        self.assertGreater(C._loo_churn(values, roles), 0.0)

    def test_separation_reports_no_split_for_role_free_distances(self):
        rng = np.random.default_rng(11)
        ids = [f"s{i}" for i in range(33)]
        base = rng.random((33, 33))
        matrix = (base + base.T) / 2
        np.fill_diagonal(matrix, 0)
        roles = {v: ("positive" if i < 23 else "contrast") for i, v in enumerate(ids)}
        result = C.separation(ids, matrix, roles, C._rng())
        self.assertFalse(result["separates"])
        self.assertGreater(result["p_permutation"], 0.05)

    def test_proximity_flags_a_far_contrast_song(self):
        ids = [f"p{i}" for i in range(10)] + ["near", "far"]
        n = len(ids)
        matrix = np.full((n, n), 0.1)
        np.fill_diagonal(matrix, 0.0)
        matrix[:, -1] = matrix[-1, :] = 5.0
        matrix[-1, -1] = 0.0
        roles = {v: "positive" for v in ids[:10]}
        roles["near"] = roles["far"] = "contrast"
        result = C.proximity(ids, matrix, roles)
        flags = {r["video_id"]: r["distant_candidate"] for r in result["contrast"]}
        self.assertTrue(flags["far"])
        self.assertFalse(flags["near"])

class GroupDistanceTests(unittest.TestCase):
    """群単位の分離はroleを見ない距離表の上で測る。

    正例だけで決めたmedoidからの距離で1次元化すると、正例が近くなるのは構成上ほぼ自明で、
    ラベルを入れ替えるpermutationでは補正できない（実際にAUC 0.177の「採用軸」が出た）。
    """

    def _songs(self, contrast_offset):
        songs = {}
        for i in range(8):
            songs[f"p{i}"] = {"views": {"arrangement": {"eligible": True, "groups": {"g": {"x": 0.5 + 0.01 * i}}}}}
        for i in range(4):
            songs[f"c{i}"] = {"views": {"arrangement": {"eligible": True, "groups": {"g": {"x": 0.5 + contrast_offset + 0.01 * i}}}}}
        return songs

    def test_identical_groups_do_not_separate(self):
        songs = self._songs(0.0)
        ids = sorted(songs)
        roles = {v: ("positive" if v.startswith("p") else "contrast") for v in ids}
        usable, matrix = C.build_group_distance("arrangement", songs, ids, "g")
        self.assertEqual(len(usable), 12)
        result = C.separation(usable, matrix, roles, C._rng())
        self.assertFalse(result["separates"])

    def test_offset_group_separates(self):
        songs = self._songs(5.0)
        ids = sorted(songs)
        roles = {v: ("positive" if v.startswith("p") else "contrast") for v in ids}
        usable, matrix = C.build_group_distance("arrangement", songs, ids, "g")
        result = C.separation(usable, matrix, roles, C._rng())
        self.assertTrue(result["separates"])
        self.assertLess(result["p_permutation"], 0.05)

    def test_group_distance_is_role_blind(self):
        songs = self._songs(1.0)
        ids = sorted(songs)
        _, a = C.build_group_distance("arrangement", songs, ids, "g")
        flipped = {vid: dict(s) for vid, s in songs.items()}
        _, b = C.build_group_distance("arrangement", flipped, ids, "g")
        self.assertTrue(np.array_equal(a, b))

if __name__ == "__main__":
    unittest.main()
