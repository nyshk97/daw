import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import schemas
from common import sha256_file
from provenance import build_stage, source_manifest, stage_source_hash, stage_valid


class ProvenanceTests(unittest.TestCase):
    def test_one_stage_source_change_only_changes_descendants_and_restores(self):
        with tempfile.TemporaryDirectory() as td:
            repo = Path(td)
            stage_paths = {s: (f"src/{s}.txt",) for s in schemas.STAGE_ORDER}
            for paths in stage_paths.values():
                for rel in paths:
                    p = repo / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(rel)
            with patch.dict("schemas.STAGE_SOURCE_PATHS", stage_paths, clear=True), patch.dict("provenance.STAGE_SOURCE_PATHS", stage_paths, clear=True):
                before = {s: stage_source_hash(s, repo) for s in schemas.STAGE_ORDER}
                target = repo / stage_paths["demucs"][0]; original = target.read_text()
                target.write_text("first edit")
                after1 = {s: stage_source_hash(s, repo) for s in schemas.STAGE_ORDER}
                target.write_text("second edit")
                after2 = {s: stage_source_hash(s, repo) for s in schemas.STAGE_ORDER}
                self.assertEqual(before["trim"], after1["trim"])
                self.assertNotEqual(before["demucs"], after1["demucs"])
                self.assertNotEqual(after1["demucs"], after2["demucs"])
                target.write_text(original)
                self.assertEqual(before, {s: stage_source_hash(s, repo) for s in schemas.STAGE_ORDER})

    def test_disposed_acquire_remains_valid_without_source(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td); source = root / "source.wav"; info = root / "source.info.json"
            source.write_bytes(b"audio"); info.write_text("{}")
            stage = build_stage("acquire", {"video_id": "x"}, {}, [
                {"path": "source.wav", "sha256": sha256_file(source), "bytes": source.stat().st_size},
                {"path": "source.info.json", "sha256": sha256_file(info), "bytes": info.stat().st_size},
            ])
            stage["materialization"] = "disposed_after_verified"; source.unlink()
            self.assertEqual(stage_valid("acquire", stage, {}, root), (True, "valid"))


if __name__ == "__main__": unittest.main()
