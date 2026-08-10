from __future__ import annotations

from pathlib import Path

STAGE_ORDER = (
    "acquire",
    "trim",
    "demucs",
    "reference_analysis",
    "taste_features",
    "distance_cluster",
)

# stage_source_hash の契約。repo相対pathの追加はここへ明示する。git状態は同一性に使わない。
STAGE_SOURCE_PATHS = {
    "acquire": (
        "tools/taste/common.py",
        "tools/reference/analyze-url.sh",
        "tools/reference/requirements.txt",
    ),
    "trim": (
        "tools/taste/common.py",
        "tools/reference/analyze-url.sh",
        "tools/reference/overview.py",
    ),
    "demucs": (
        "tools/reference/analyze.py",
        "tools/reference/analyze.sh",
        "tools/reference/requirements.txt",
    ),
    "reference_analysis": (
        "tools/reference/analyze.py",
        "tools/reference/arrange.py",
        "tools/reference/basics.py",
        "tools/reference/gates.py",
        "tools/reference/groove.py",
        "tools/reference/stems.py",
        "tools/reference/topline.py",
        "tools/reference/requirements.txt",
    ),
    "taste_features": (
        "tools/taste/common.py",
        "tools/taste/features.py",
        "tools/taste/grid-audit.py",
        "tools/taste/segments.py",
        "tools/taste/shared_features.py",
    ),
    "distance_cluster": (
        "tools/taste/cluster.py",
        "tools/taste/distances.py",
    ),
}

STAGE_PARENTS = {
    "acquire": (),
    "trim": ("acquire",),
    "demucs": ("trim",),
    "reference_analysis": ("trim", "demucs"),
    "taste_features": ("reference_analysis",),
    "distance_cluster": ("taste_features",),
}

PER_SONG_STAGES = ("acquire", "trim", "demucs", "reference_analysis")
GRID_STATUSES = {
    "needs_review",
    "auto_verified_4_4",
    "human_verified_4_4",
    "human_verified_non_4_4",
    "unknown",
}
METER_DEPENDENT_ELIGIBLE = {"auto_verified_4_4", "human_verified_4_4"}
MATERIALIZATION_STATES = {
    "present",
    "dispose_pending",
    "disposed_after_verified",
    "missing_unexpected",
}

VIEW_SCHEMAS = {
    "topline_harmony": {
        "meter_dependent": True,
        "required_groups": ["harmony", "motion", "instruments", "texture"],
        "weights": {"harmony": 0.35, "motion": 0.2, "instruments": 0.15, "texture": 0.3},
    },
    "drum_placement": {
        "meter_dependent": True,
        "required_groups": ["profiles", "timing"],
        "weights": {"profiles": 0.75, "timing": 0.25},
    },
    "drum_audio": {
        "meter_dependent": False,
        "required_groups": ["hit_character", "production"],
        "weights": {"hit_character": 0.6, "production": 0.4},
    },
    "bass_harmony": {
        "meter_dependent": True,
        "required_groups": ["placement", "pitch_relation", "density", "motion"],
        "weights": {"placement": 0.25, "pitch_relation": 0.3, "density": 0.15, "motion": 0.3},
    },
    "arrangement": {
        "meter_dependent": True,
        "required_groups": ["occupancy", "shape", "section_sequence"],
        "weights": {"occupancy": 0.3, "shape": 0.3, "section_sequence": 0.4},
    },
}

OVERALL_MANDATORY_VIEWS = list(VIEW_SCHEMAS)


def undeclared_taste_sources(repo_root: Path) -> list[str]:
    """tools/tasteの実装pyのうちstage source schemaから漏れたものを返す。"""
    declared = {p for paths in STAGE_SOURCE_PATHS.values() for p in paths}
    ignored = {"tools/taste/__init__.py"}
    actual = {
        p.relative_to(repo_root).as_posix()
        for p in (repo_root / "tools/taste").glob("*.py")
        if p.name != "__init__.py"
    }
    return sorted(actual - declared - ignored)
