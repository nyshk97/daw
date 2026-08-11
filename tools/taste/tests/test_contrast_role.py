import json
import tempfile
import unittest
from pathlib import Path

import numpy as np

from cluster import build_distance
from corpus import parse_all_picks, sync_manifest
from schemas import PER_SONG_STAGES, STAGE_SOURCE_PATHS


def _arrangement_song(occupancy, section_count, seq):
    return {
        "views": {
            "arrangement": {
                "eligible": True,
                "groups": {
                    "occupancy": occupancy,
                    "shape": {"section_count": section_count, "mean_length_bars": 8.0, "reentry_count": 2},
                    "section_sequence": seq,
                },
            }
        }
    }


class ContrastRoleTests(unittest.TestCase):
    def test_two_picks_files_assign_roles(self):
        with tempfile.TemporaryDirectory() as td:
            positive = Path(td) / "picks.md"
            contrast = Path(td) / "contrast.md"
            positive.write_text("- A https://youtu.be/12345678901\n")
            contrast.write_text("- B https://youtu.be/ABCDEFGHIJK\n- C https://youtu.be/ZYXWVUTSRQP\n")
            entries, diagnostics, blocking = parse_all_picks(positive, contrast)
            self.assertEqual(diagnostics, [])
            self.assertFalse(blocking)
            self.assertEqual(
                {e["video_id"]: e["role"] for e in entries},
                {"12345678901": "positive", "ABCDEFGHIJK": "contrast", "ZYXWVUTSRQP": "contrast"},
            )
            self.assertTrue(all(e["picks_source"] for e in entries))

    def test_missing_contrast_file_is_positive_only(self):
        with tempfile.TemporaryDirectory() as td:
            positive = Path(td) / "picks.md"
            positive.write_text("- A https://youtu.be/12345678901\n")
            entries, diagnostics, blocking = parse_all_picks(positive, Path(td) / "absent.md")
            self.assertEqual([e["role"] for e in entries], ["positive"])
            self.assertEqual(diagnostics, [])
            self.assertFalse(blocking)

    def test_role_conflict_blocks_manifest_write(self):
        with tempfile.TemporaryDirectory() as td:
            root, search = Path(td) / "corpus", Path(td) / "search"
            search.mkdir()
            positive = Path(td) / "picks.md"
            contrast = Path(td) / "contrast.md"
            positive.write_text("- A https://youtu.be/12345678901\n")
            contrast.write_text("- A別名 https://youtu.be/12345678901\n")
            manifest, diagnostics = sync_manifest(positive, root, search, False, contrast)
            self.assertTrue(any("role_conflict" in d for d in diagnostics))
            self.assertFalse((root / "manifest.json").exists())
            self.assertEqual(manifest["songs"]["12345678901"]["role"], "positive")

    def test_adding_contrast_songs_keeps_positive_entries_intact(self):
        with tempfile.TemporaryDirectory() as td:
            root, search = Path(td) / "corpus", Path(td) / "search"
            search.mkdir()
            positive = Path(td) / "picks.md"
            contrast = Path(td) / "contrast.md"
            positive.write_text("- A https://youtu.be/12345678901\n")
            first, _ = sync_manifest(positive, root, search, False, None)
            first["songs"]["12345678901"].update(status="analyzed", grid={"grid_status": "auto_verified_4_4"})
            (root / "manifest.json").write_text(json.dumps(first, ensure_ascii=False))

            contrast.write_text("- B https://youtu.be/ABCDEFGHIJK\n")
            second, _ = sync_manifest(positive, root, search, False, contrast)
            kept = second["songs"]["12345678901"]
            self.assertEqual(kept["status"], "analyzed")
            self.assertEqual(kept["grid"], {"grid_status": "auto_verified_4_4"})
            self.assertEqual(kept["active_artifact_path"], first["songs"]["12345678901"]["active_artifact_path"])
            self.assertEqual(second["songs"]["ABCDEFGHIJK"]["status"], "pending")
            self.assertEqual(second["role_counts"], {"positive": 1, "contrast": 1})

    def test_input_contract_module_is_outside_per_song_stage_hashes(self):
        """picksのpath定数を足しただけで既存曲の取得・トリム成果物をinvalidにしない。

        common.pyへ定数を1つ足したときに23曲がacquireから再実行対象になった実例があるため、
        入力契約はinputs.pyへ分け、per-song stageのsource hash対象に入れない契約を固定する。
        """
        for stage in PER_SONG_STAGES:
            self.assertNotIn("tools/taste/inputs.py", STAGE_SOURCE_PATHS[stage], stage)

    def test_distance_table_is_role_invariant(self):
        songs = {
            "aaaaaaaaaaa": _arrangement_song({"drums": 0.9, "bass": 0.8}, 5, [{"length_ratio": 0.5, "active": ["drums"]}]),
            "bbbbbbbbbbb": _arrangement_song({"drums": 0.4, "bass": 0.3}, 9, [{"length_ratio": 0.2, "active": ["bass"]}]),
            "ccccccccccc": _arrangement_song({"drums": 0.6, "bass": 0.6}, 7, [{"length_ratio": 0.3, "active": ["drums", "bass"]}]),
        }
        labelled = {vid: {**s, "role": r} for (vid, s), r in zip(songs.items(), ["positive", "contrast", "positive"])}
        flipped = {vid: {**s, "role": r} for (vid, s), r in zip(songs.items(), ["contrast", "positive", "contrast"])}
        ids_a, matrix_a, _ = build_distance("arrangement", labelled)
        ids_b, matrix_b, _ = build_distance("arrangement", flipped)
        self.assertEqual(ids_a, ids_b)
        self.assertTrue(np.array_equal(matrix_a, matrix_b))

class CommonCoreScopeTests(unittest.TestCase):
    """共通核は正例だけで数える。否定例を混ぜると母数の意味が変わる。"""

    def _songs(self):
        def song(role, eligible):
            return {
                "role": role,
                "grid_status": "auto_verified_4_4",
                "views": {"drum_audio": {"eligible": eligible}, "topline_harmony": {"eligible": eligible}},
            }
        return {"p1": song("positive", True), "p2": song("positive", True), "c1": song("contrast", False)}

    def test_contrast_songs_do_not_change_the_denominator(self):
        from cluster import _common_core
        core = _common_core(self._songs())
        self.assertEqual(core["positive_total"], 2)
        names = {f["feature"] for f in core["facts"]}
        # 否定例だけがineligibleでも、正例の共通核は残る
        self.assertIn("drum_audio_measurable", names)
        for fact in core["facts"]:
            self.assertEqual(fact["measured"], "2/2")
            self.assertEqual(fact["support"], "2/2")

    def test_an_ineligible_positive_removes_the_fact(self):
        from cluster import _common_core
        songs = self._songs()
        songs["p2"]["views"]["drum_audio"]["eligible"] = False
        names = {f["feature"] for f in _common_core(songs)["facts"]}
        self.assertNotIn("drum_audio_measurable", names)

if __name__ == "__main__":
    unittest.main()
