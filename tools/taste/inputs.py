"""コーパスの入力契約（picksファイルとrole）。

`common.py` へ置かない理由: common.pyは `acquire` / `trim` stageの `stage_source_hash` 対象で、
そこへ定数を1つ足すだけで既存曲の取得・トリム成果物が全部invalidになる。picksのpathとroleは
取得やトリムの挙動を一切変えないので、fingerprintを揺らさない別モジュールに置く。
"""

from __future__ import annotations

from common import REPO_ROOT

CONTRAST_PICKS_PATH = REPO_ROOT / "docs/labs/reference-beat-contrast-picks.md"

# roleは解釈用のラベル。featureにも距離にも渡さない（アーティスト名と同じ循環防止）。
ROLES = ("positive", "contrast")
