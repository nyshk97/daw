import unittest

from distances import circular_pitch_distance, dtw_distance, jaccard, jensen_shannon, section_distance, symmetric_nearest_set_distance


class DistanceTests(unittest.TestCase):
    def test_distribution_multihot_and_pitch(self):
        self.assertEqual(jensen_shannon([0, 0], [0, 0]), 0)
        self.assertEqual(jensen_shannon([0, 0], [1, 0]), 1)
        self.assertEqual(jaccard([], []), 0)
        self.assertAlmostEqual(circular_pitch_distance(11, 0), 1/6)

    def test_code_loop_rotation_but_not_reverse(self):
        a = [0, 2, 5, 9]
        self.assertEqual(dtw_distance(a, [5, 9, 0, 2], circular_pitch_distance, cyclic=True), 0)
        self.assertGreater(dtw_distance(a, list(reversed(a)), circular_pitch_distance, cyclic=True), 0)

    def test_section_sequence_is_not_rotated(self):
        a = [{"active": ["drums"], "length_ratio": .5}, {"active": ["bass"], "length_ratio": .5}]
        self.assertGreater(dtw_distance(a, list(reversed(a)), section_distance, cyclic=False), 0)

    def test_two_vs_three_segments_each_song_has_equal_set_weight(self):
        weights = {"x": 1.0}
        a = [{"x": {"profile": [1, 0]}}, {"x": {"profile": [0.9, .1]}}]
        b = [{"x": {"profile": [0, 1]}}, {"x": {"profile": [.1, .9]}}, {"x": {"profile": [.2, .8]}}]
        d1, _ = symmetric_nearest_set_distance(a, b, weights)
        d2, _ = symmetric_nearest_set_distance(b, a, weights)
        self.assertAlmostEqual(d1, d2)


if __name__ == "__main__": unittest.main()
