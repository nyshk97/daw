import tempfile
import unittest
from pathlib import Path

import numpy as np
import soundfile as sf

import features as F
from schemas import OVERALL_MANDATORY_VIEWS, VIEW_SCHEMAS
from segments import Candidate


def _tone(hz, seconds, sr=F.SR, amp=0.3):
    t = np.arange(int(sr * seconds)) / sr
    return (amp * np.sin(2 * np.pi * hz * t)).astype(np.float32)


def _gated(hz, seconds, on=0.25, off=0.25):
    y = _tone(hz, seconds)
    period = int(F.SR * (on + off))
    for start in range(int(F.SR * on), len(y), period):
        y[start : start + int(F.SR * off)] = 0
    return y


class VocalSpaceTests(unittest.TestCase):
    def test_continuous_band_has_no_gap_and_full_run(self):
        y = _tone(1000, 4.0)
        g = F.vocal_space_group(y, y, np.zeros_like(y), np.zeros_like(y))
        self.assertLess(g["time_space"]["gap_ratio"], 0.05)
        self.assertGreater(g["time_space"]["max_fill_ratio"], 0.9)

    def test_gated_band_opens_space(self):
        continuous = F.vocal_space_group(_tone(1000, 4.0), _tone(1000, 4.0), np.zeros(int(F.SR * 4), np.float32), np.zeros(int(F.SR * 4), np.float32))
        gated_y = _gated(1000, 4.0)
        gated = F.vocal_space_group(gated_y, gated_y, np.zeros_like(gated_y), np.zeros_like(gated_y))
        self.assertGreater(gated["time_space"]["gap_ratio"], continuous["time_space"]["gap_ratio"] + 0.2)
        self.assertLess(gated["time_space"]["max_fill_ratio"], continuous["time_space"]["max_fill_ratio"])

    def test_band_ratio_separates_in_band_from_out_of_band(self):
        inside = _tone(1000, 3.0)
        outside = _tone(60, 3.0)
        hi = F.vocal_space_group(inside, inside, np.zeros_like(inside), np.zeros_like(inside))
        lo = F.vocal_space_group(outside, outside, np.zeros_like(outside), np.zeros_like(outside))
        self.assertGreater(hi["band_occupancy"]["vocal_band_ratio"], 0.9)
        self.assertLess(lo["band_occupancy"]["vocal_band_ratio"], 0.1)

    def test_masking_shares_attribute_band_energy_to_the_right_stem(self):
        drums = _tone(1200, 3.0)
        bass = _tone(55, 3.0)
        top = np.zeros(int(F.SR * 3), np.float32)
        g = F.vocal_space_group(drums + bass + top, drums, bass, top)
        shares = g["masking_source"]["masking_shares"]
        self.assertAlmostEqual(sum(shares), 1.0, places=4)
        self.assertGreater(shares[0], 0.95)
        self.assertLess(shares[1], 0.05)

    def test_require_active_drops_windows_missing_a_part(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            # 0-4秒はtopだけ、4-8秒は3パートとも鳴る。
            silence = np.zeros(int(F.SR * 4), np.float32)
            top = np.concatenate([_tone(800, 4.0), _tone(800, 4.0)])
            drums = np.concatenate([silence, _tone(1500, 4.0)])
            bass = np.concatenate([silence, _tone(70, 4.0)])
            paths = {}
            for name, y in (("top", top), ("drums", drums), ("bass", bass)):
                p = root / f"{name}.wav"
                sf.write(p, y, F.SR)
                paths[name] = [p]
            beat = paths["drums"] + paths["bass"] + paths["top"]
            candidates = [Candidate(1, 5, 0.0, 4.0, {}, ()), Candidate(5, 9, 4.0, 8.0, {}, ())]
            without = F._segments_for_view("vocal_space", beat, candidates)
            withreq = F._segments_for_view(
                "vocal_space", beat, candidates,
                require_active=[paths["drums"], paths["top"]],
                require_low_band=True,
            )
            self.assertEqual(len(without), 2)
            self.assertEqual([s["start_bar"] for s in withreq], [5])

    def test_low_band_is_measured_on_the_mix_not_the_bass_stem(self):
        """Demucsが低域をdrums/otherへ振り分けてもbass有りと判定できること。

        bass stemが-72dBなのに曲には低域が75%あるtype beatが実在した。
        stemのRMSで低音の在否を決めると、その曲だけvocal_spaceから落ちる。
        """
        low = _tone(60, 3.0, amp=0.5)
        top = _tone(900, 3.0, amp=0.2)
        self.assertGreater(F._low_band_share(low + top), F.MIN_LOW_BAND_SHARE)
        self.assertLess(F._low_band_share(top), F.MIN_LOW_BAND_SHARE)

    def test_vocal_space_is_not_part_of_the_overall_mandatory_views(self):
        self.assertIn("vocal_space", VIEW_SCHEMAS)
        self.assertNotIn("vocal_space", OVERALL_MANDATORY_VIEWS)
        self.assertEqual(
            OVERALL_MANDATORY_VIEWS,
            ["topline_harmony", "drum_placement", "drum_audio", "bass_harmony", "arrangement"],
        )

class DrumShapeTests(unittest.TestCase):
    """16分profileから「刻みの細かさ」「規則的でゆったり」を取り出せること。"""

    def _profile(self, positions, value=1.0):
        p = [0.0] * 16
        for i in positions:
            p[i] = value
        return p

    def test_kick_on_quarter_separates_regular_from_scattered(self):
        regular = F.drum_shape({"low": self._profile([0, 4, 8, 12]), "mid": self._profile([4, 12]), "high": self._profile([0, 2, 4, 6, 8, 10, 12, 14])})
        scattered = F.drum_shape({"low": self._profile([0, 3, 6, 7, 10, 14]), "mid": self._profile([4, 12]), "high": self._profile([0, 2, 4, 6, 8, 10, 12, 14])})
        self.assertEqual(regular["kick_on_quarter"], 1.0)
        self.assertLess(scattered["kick_on_quarter"], 0.4)
        self.assertLess(regular["kick_spread"], scattered["kick_spread"])

    def test_hat_sixteenth_separates_eighth_from_sixteenth_hats(self):
        eighth = F.drum_shape({"low": self._profile([0, 8]), "mid": self._profile([4, 12]), "high": self._profile([0, 2, 4, 6, 8, 10, 12, 14])})
        sixteenth = F.drum_shape({"low": self._profile([0, 8]), "mid": self._profile([4, 12]), "high": self._profile(range(16))})
        self.assertAlmostEqual(eighth["hat_sixteenth"], 0.0, places=6)
        self.assertAlmostEqual(sixteenth["hat_sixteenth"], 0.5, places=6)
        self.assertLess(eighth["hat_spread"], sixteenth["hat_spread"])

    def test_snare_backbeat_is_one_for_a_plain_backbeat(self):
        shape = F.drum_shape({"low": self._profile([0, 8]), "mid": self._profile([4, 12]), "high": self._profile([0, 8])})
        self.assertAlmostEqual(shape["snare_backbeat"], 1.0, places=6)

    def test_silent_band_does_not_crash(self):
        shape = F.drum_shape({"low": [0.0] * 16, "mid": self._profile([4, 12]), "high": self._profile([0, 8])})
        self.assertEqual(shape["kick_on_quarter"], 0.0)
        self.assertEqual(shape["kick_spread"], 0.0)

if __name__ == "__main__":
    unittest.main()
