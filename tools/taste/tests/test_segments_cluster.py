import unittest

import numpy as np

from cluster import _cluster, _output_schema_revision, candidates_for_view
from segments import Candidate, select_representative_diverse


def group(v):
    return {
        "harmony": {"root_sequence": [v, (v + 2) % 12], "root_interval_hist": [1] + [0]*11, "change_ratio": .5, "third_ambiguous_ratio": .1, "color_chord_ratio": .5, "return_ratio": 0, "confidence": .8},
        "motion": {"onset_rate": 1 + v/100, "repetition": .5},
        "instruments": ["guitar"],
        "texture": {"band_balance": [.1,.1,.5,.2,.1], "centroid_hz": 1000+v, "rolloff95_hz": 3000+v, "hpss_ratio": 2},
    }


class SegmentClusterTests(unittest.TestCase):
    def test_selection_is_deterministic_and_keeps_diversity(self):
        c = [Candidate(i, i+7, i*2, i*2+8, {}, ()) for i in range(4)]
        one = select_representative_diverse(c, [10, 9, 8, 7], [[0], [.1], [10], [9]], 3)
        two = select_representative_diverse(c, [10, 9, 8, 7], [[0], [.1], [10], [9]], 3)
        self.assertEqual(one, two)
        self.assertEqual(one[-1][1], "diversity")
        self.assertIn(one[-1][0], {2, 3})

    def test_m3_singleton_partition_is_rejected(self):
        ids = ["a", "b", "c"]
        matrix = np.array([[0,.1,.9],[.1,0,.8],[.9,.8,0]])
        songs = {v: {"views": {"topline_harmony": {"eligible": True, "segments": [{"eligible": True, "groups": group(i)}]}}} for i, v in enumerate(ids)}
        self.assertEqual(candidates_for_view("topline_harmony", ids, matrix, songs), [])

    def test_average_precomputed_cluster_is_deterministic(self):
        matrix = np.array([[0,.1,.8,.9],[.1,0,.9,.8],[.8,.9,0,.1],[.9,.8,.1,0]])
        self.assertEqual(_cluster(matrix, 2).tolist(), _cluster(matrix, 2).tolist())

    def test_cluster_output_inherits_feature_schema_revision(self):
        self.assertEqual(_output_schema_revision({"schema_revision": 2}), 2)
        self.assertEqual(_output_schema_revision({}), 1)


if __name__ == "__main__": unittest.main()
