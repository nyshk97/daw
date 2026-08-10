import unittest

import numpy as np

from features import SR, bass_motion_group
from schemas import VIEW_SCHEMAS


class BassMotionTests(unittest.TestCase):
    def test_pitch_motion_separates_static_and_moving_bass(self):
        seconds = 8
        t = np.arange(int(SR * seconds)) / SR
        static = np.sin(2 * np.pi * 110.0 * t).astype(np.float32)
        moving = np.zeros_like(static)
        notes = [110.0, 146.83, 196.0, 146.83]
        block = int(SR * 0.5)
        for i, start in enumerate(range(0, len(moving), block)):
            end = min(len(moving), start + block)
            local_t = np.arange(end - start) / SR
            moving[start:end] = np.sin(2 * np.pi * notes[i % len(notes)] * local_t)
        static_features = bass_motion_group(static, 90.0)
        moving_features = bass_motion_group(moving, 90.0)
        self.assertGreater(moving_features["pitch_motion"], static_features["pitch_motion"] + 0.01)

    def test_bass_schema_requires_motion_with_normalized_weights(self):
        schema = VIEW_SCHEMAS["bass_harmony"]
        self.assertIn("motion", schema["required_groups"])
        self.assertAlmostEqual(sum(schema["weights"].values()), 1.0)


if __name__ == "__main__":
    unittest.main()
