import json
import tempfile
import unittest
from pathlib import Path

from corpus import parse_picks, sync_manifest


class CorpusTests(unittest.TestCase):
    def test_markdown_variations_and_diagnostics(self):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "picks.md"
            path.write_text(
                "# list\n"
                "- 曲名　https://youtu.be/12345678901\n"
                "- 記号 × 曲 -https://www.youtube.com/watch?v=ABCDEFGHIJK\n"
                "- 15MUS　-　YourSong https://www.youtube.com/watch?v=ZYXWVUTSRQP\n"
                "- URLなし\n"
                "- 別名 https://youtu.be/12345678901\n"
            )
            entries, diagnostics = parse_picks(path)
            self.assertEqual([e["display_name"] for e in entries], ["曲名", "記号 × 曲", "15MUS　-　YourSong"])
            self.assertEqual(len(diagnostics), 2)
            self.assertIn("URLなし", diagnostics[0])
            self.assertIn("別名", diagnostics[1])

    def test_sync_add_and_orphan_without_delete(self):
        with tempfile.TemporaryDirectory() as td:
            root, search = Path(td) / "corpus", Path(td) / "search"
            search.mkdir()
            picks = Path(td) / "picks.md"
            picks.write_text("- A https://youtu.be/12345678901\n- B https://youtu.be/ABCDEFGHIJK\n")
            first, _ = sync_manifest(picks, root, search)
            artifact = Path(first["songs"]["ABCDEFGHIJK"]["active_artifact_path"])
            artifact.mkdir(parents=True); marker = artifact / "keep"; marker.write_text("x")
            picks.write_text("- A https://youtu.be/12345678901\n")
            second, _ = sync_manifest(picks, root, search)
            self.assertIn("ABCDEFGHIJK", second["orphans"])
            self.assertTrue(marker.exists())

    def test_legacy_external_is_conflict_and_untouched(self):
        with tempfile.TemporaryDirectory() as td:
            root, search = Path(td) / "corpus", Path(td) / "music"
            ext = search / "project/references/song"; ext.mkdir(parents=True)
            info = ext / "source.info.json"; info.write_text(json.dumps({"id": "12345678901", "title": "A"}))
            before = info.stat().st_mtime_ns
            picks = Path(td) / "picks.md"; picks.write_text("- A https://youtu.be/12345678901\n")
            manifest, _ = sync_manifest(picks, root, search)
            song = manifest["songs"]["12345678901"]
            self.assertEqual(song["ownership"], "corpus")
            self.assertEqual(song["reuse_conflict"], "legacy_or_incomplete_provenance")
            self.assertEqual(info.stat().st_mtime_ns, before)


if __name__ == "__main__": unittest.main()
