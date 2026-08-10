import tempfile
import unittest
from pathlib import Path

import numpy as np
import soundfile as sf

from importlib.util import module_from_spec, spec_from_file_location

root = Path(__file__).parents[1]
review_spec = spec_from_file_location("review", root / "review.py"); review = module_from_spec(review_spec); review_spec.loader.exec_module(review)
followup_spec = spec_from_file_location("review_followup", root / "review-followup.py"); followup = module_from_spec(followup_spec); followup_spec.loader.exec_module(followup)
sense_spec = spec_from_file_location("sensitivity", root / "comparison-sensitivity.py"); sensitivity = module_from_spec(sense_spec); sense_spec.loader.exec_module(sensitivity)


class ReviewSensitivityTests(unittest.TestCase):
    def test_followup_set_is_small_and_has_no_duplicate_view_song(self):
        pairs = [(spec["view"], spec["video_id"]) for spec in followup.FOLLOWUP_SPECS]
        self.assertEqual(len(pairs), len(set(pairs)))
        self.assertEqual(followup.expected_file_count(), 14)
        self.assertEqual(sum(spec["view"] == "drum_audio" for spec in followup.FOLLOWUP_SPECS), 4)
        self.assertEqual(sum(len(spec["kinds"]) for spec in followup.FOLLOWUP_SPECS if spec["view"] == "bass_harmony"), 8)

    def test_review_uses_only_accepted_cluster_candidates(self):
        rejected = {"accepted": False, "labels": {"a": 0, "b": 1}}
        accepted = {"accepted": True, "labels": {"a": 0, "b": 0}}
        self.assertIsNone(review._accepted_candidate([rejected]))
        self.assertIs(review._accepted_candidate([rejected, accepted]), accepted)

    def test_review_wav_self_inspection(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "ok.wav"
            sr = 22050; sf.write(path, np.sin(2*np.pi*440*np.arange(sr*2)/sr)*.1, sr)
            stats = review.inspect_wav(path)
            self.assertGreater(stats["duration_s"], 1)
            silent = Path(td) / "silent.wav"; sf.write(silent, np.zeros(sr*2), sr)
            with self.assertRaises(ValueError): review.inspect_wav(silent)

    def test_sensitivity_requires_three_stable_groups(self):
        rows = []
        for i in range(8):
            feat = {"harmony": {"x": i/10}, "pitch_motion_repetition": {"x": i/10}, "onset_rhythm": {"x": i/10}, "spectrum_texture": {"band_balance": [.2,.8]}}
            rows.append({"clean": feat, "separated": feat})
        result = sensitivity.evaluate(rows)
        self.assertTrue(result["comparison_supported"])


if __name__ == "__main__": unittest.main()
