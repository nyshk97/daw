#!/usr/bin/env python
"""リファレンス1曲ぶんの分析を、依存グラフに沿って並列に走らせるオーケストレータ。

かつては analyze.sh がステップを直列に流していて約4分45秒かかっていた（実測・4:35の曲）。
ボトルネックは demucs（GPU・計48秒）ではなく librosa 系 CPU ステップ（計約230秒）が
直列に並んでいたことなので、依存が満たされたステップから順に並走させる。
各ステップの計算内容・パラメータは一切変えていない＝出力は直列版と同一。

設計メモ:
  - CPU の同時実行は「ステップ数」でなく「重み（≒そのステップが内部で使うワーカー数）」の
    合計で予算に収める。無制限に起動すると NumPy 内部スレッドも重なって
    単体実測の合算より遅くなる。demucs は GPU 主体なので重み 0
  - demucs 2モデルは GPU の取り合いになるだけなので直列（demucs_6s が demucs_4s に依存）
  - 子プロセスの stdout/stderr は起動直後から drain スレッドで読み続ける。
    PIPE を完了まで読まないとバッファ満杯でデッドロックする（groove.py 等は
    大きな JSON を stdout に出す）。text=True の universal newlines で
    tqdm の \r 進捗も行として流れる
  - LaLa の進捗ダイアログは stdout の最新行を1行表示するだけなので、
    こちらからは「==> 実行中: A・B・C」の集約行と card.py の申告行だけを stdout に流す。
    失敗の詳細は stderr（ReferenceAnalyzer は失敗理由を stderr からしか抽出しない）
  - fail-fast / キャンセルの停止手順: 各ステップを自分のプロセスグループのリーダーとして
    起動し（process_group=0）、停止は killpg で行う。ppid をたどる方式は「親が先に死んで
    孫が reparent される」「失敗したステップ自身の子孫は親の死後に列挙できない」の2パターンで
    取りこぼすため使えない。ステップが別グループになると LaLa のキャンセル（orchestrator の
    グループへ killpg TERM → 1秒後 KILL）が直接は届かなくなるので、orchestrator が SIGTERM を
    例外に変換して stop_all で全ステップグループへ TERM → 0.5秒 → KILL を伝播する
    （1秒の猶予内に撃ち切る）

使い方: analyze.py <リファレンスフォルダ>
前提: フォルダに track.wav（MVエディットをトリム済みの本編）が置いてあること。
トリム位置の判断だけは自動化しない（MVエディットの入り方は曲ごとに違う）。
"""
import os
import signal
import subprocess
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
# 論理10コア（P コア4）の環境で飽和しない値。
# 重みはステップが内部で使うワーカー数に合わせる（stems=3・groove=2・他=1・demucs=0）
CPU_BUDGET = 6

# librosa 系ステップは BLAS のマルチスレッドが純粋な足かせ（実測: pyin 単独でも
# スレッド1本の方が速く、並走時は sys time が爆発して 52秒→76秒に劣化する。
# 数値結果はスレッド数によらずバイト一致を確認済み）。demucs（torch/MPS）には適用しない
THREAD_CAP_ENV = {
    "OMP_NUM_THREADS": "1",
    "OPENBLAS_NUM_THREADS": "1",
    "VECLIB_MAXIMUM_THREADS": "1",
    "MKL_NUM_THREADS": "1",
}

# stdout（進捗）に転送する子プロセスの行。card.py の「カードを生成しない（〜）」申告は
# LaLa が完了ダイアログ用に stdout から拾うので落とせない
FORWARD_STDOUT_MARKERS = ("カードを生成しない",)


@dataclass
class Step:
    name: str
    label: str  # 進捗表示用（「ステム分離(4s)」等）
    cmd: list
    deps: tuple = ()
    weight: int = 1  # CPU 予算の消費量。demucs（GPU 主体）は 0
    priority: int = 50  # 小さいほど先に起動（予算が足りないとき長いステップを優先する）
    skip: object = None  # callable() -> str | None。文字列を返したらスキップ（理由として表示）
    env: dict = None  # 追加の環境変数（os.environ にマージ。ワーカープロセスにも継承される）


@dataclass
class Running:
    step: Step
    proc: subprocess.Popen
    t0: float = field(default_factory=time.monotonic)
    stdout_tail: deque = field(default_factory=lambda: deque(maxlen=50))
    stderr_tail: deque = field(default_factory=lambda: deque(maxlen=50))
    threads: list = field(default_factory=list)


class StepFailed(Exception):
    def __init__(self, running: Running):
        self.running = running
        super().__init__(f"{running.step.label} failed (exit {running.proc.returncode})")


def _drain(stream, tail: deque, forward) -> None:
    for line in stream:
        line = line.rstrip("\n")
        if line.strip():
            tail.append(line[:500])
            if forward is not None:
                forward(line)
    stream.close()


def _group_alive(pgid: int) -> bool:
    """ステップのプロセスグループにまだ誰か居るか。

    killpg(pgid, 0) は使わない: macOS はグループ内に1つでもシグナルを送れない状態の
    メンバー（終了処理中等）がいると EPERM を返し、生死の判定にならない。
    """
    return subprocess.run(["pgrep", "-g", str(pgid)], capture_output=True).returncode == 0


def _killpg_all(pgids: list, sig: int) -> None:
    for pgid in pgids:
        try:
            os.killpg(pgid, sig)
        except (ProcessLookupError, PermissionError):
            pass  # 全滅済み / 終了処理中のメンバーが混ざっている（macOS の EPERM）


def stop_all(runnings: list) -> None:
    """実行中ステップを子孫まで含めて確実に止める。

    各ステップは自分のプロセスグループのリーダー（launch の process_group=0）なので、
    killpg でワーカー・孫まで一網打尽にできる。ppid をたどる方式だと「親が先に死んで
    孫が reparent される」「失敗したステップ自身の子孫は親の死後に列挙できない」の
    2パターンで取りこぼす（実際に取りこぼした）ため、グループ単位でしか安全に消せない。
    LaLa キャンセル時（SIGTERM 後 1秒で SIGKILL が来る）にも間に合うよう、
    全グループへ一斉に TERM → 短い猶予 → KILL の順で撃つ。
    """
    groups = [r.proc.pid for r in runnings]  # pgid == 直接の子の pid
    # 猶予は「TERM 0.5秒 → KILL」。LaLa の SIGKILL が 1秒後に来るため、キャンセル経路では
    # ここを 1秒以内に撃ち切らないと TERM を無視するプロセスが残る
    _killpg_all(groups, signal.SIGTERM)
    deadline = time.monotonic() + 0.5
    while time.monotonic() < deadline:
        for r in runnings:
            r.proc.poll()  # 直接の子はゾンビのままだとグループに残り続けるので回収する
        if not any(_group_alive(g) for g in groups):
            break
        time.sleep(0.05)
    alive = [g for g in groups if _group_alive(g)]
    if alive:
        _killpg_all(alive, signal.SIGKILL)  # SIGTERM を無視する孫ごと落とす
        deadline = time.monotonic() + 0.4
        while time.monotonic() < deadline and any(_group_alive(g) for g in alive):
            for r in runnings:
                r.proc.poll()
            time.sleep(0.05)
    for r in runnings:
        if r.proc.poll() is None:
            r.proc.kill()
        r.proc.wait()
        for t in r.threads:
            t.join(timeout=2)


def run_dag(steps: list, budget: int = CPU_BUDGET, echo=None) -> None:
    """依存が満たされ、重み合計が予算に収まるステップから起動する。

    1ステップでも失敗したら実行中の全ステップを止めて StepFailed を投げる（fail-fast）。
    echo(line) は進捗の集約行の出力先（既定は stdout へ flush 付き print）。
    """
    if echo is None:
        echo = lambda line: print(line, flush=True)  # noqa: E731

    # LaLa のキャンセルは orchestrator への SIGTERM（1秒後にグループへ SIGKILL）。
    # ステップは別プロセスグループなので orchestrator が黙って死ぬとステップが孤児で残る
    # → 例外に変換して except の stop_all に流し、1秒の猶予内に全グループを掃除する。
    # SIGHUP（ターミナルを閉じた等）も同じ経路に流す（別グループのステップには届かないため）。
    #
    # ただし Popen（子グループ作成）から running への登録までの区間で即 raise すると、
    # 登録前の子が stop_all に渡らず孤児化する。この区間はフラグだけ立てて、
    # 登録が終わった直後のチェックで raise する（sigmask で塞ぐ案は、ブロック中に
    # fork/exec した子がマスクを継承して TERM が効かなくなる副作用があるので使わない）
    # 依存の検証はハンドラ設定より前に行う（設定後に try/finally の外で raise すると
    # ハンドラが復元されないまま戻ってしまう）
    by_name = {s.name: s for s in steps}
    for s in steps:
        for d in s.deps:
            if d not in by_name:
                raise ValueError(f"step {s.name} depends on unknown step {d}")

    cancelled = False
    in_launch = False

    def _on_term(signum, frame):
        nonlocal cancelled
        cancelled = True
        if not in_launch:
            raise SystemExit(143)

    prev_term = signal.signal(signal.SIGTERM, _on_term)
    prev_hup = signal.signal(signal.SIGHUP, _on_term)

    done: set = set()
    pending = list(steps)
    running: dict = {}
    last_status = ""

    def used() -> int:
        return sum(r.step.weight for r in running.values())

    def announce() -> None:
        nonlocal last_status
        labels = "・".join(r.step.label for r in running.values())
        status = f"==> 実行中: {labels}" if labels else ""
        if status and status != last_status:
            last_status = status
            echo(status)

    def forward_stdout(line: str) -> None:
        if any(m in line for m in FORWARD_STDOUT_MARKERS):
            print(line, flush=True)

    def launch(step: Step) -> None:
        env = None
        if step.env:
            env = dict(os.environ)
            env.update(step.env)
        proc = subprocess.Popen(
            step.cmd,
            cwd=HERE,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            process_group=0,  # ステップを自グループのリーダーに（stop_all の killpg 用）
        )
        r = Running(step, proc)
        t_out = threading.Thread(target=_drain, args=(proc.stdout, r.stdout_tail, forward_stdout), daemon=True)
        t_err = threading.Thread(target=_drain, args=(proc.stderr, r.stderr_tail, None), daemon=True)
        t_out.start()
        t_err.start()
        r.threads = [t_out, t_err]
        running[step.name] = r

    try:
        while pending or running:
            if cancelled:
                raise SystemExit(143)
            # 起動できるものをすべて起動（priority 順。重み 0 = GPU は予算外）
            ready = [s for s in pending if set(s.deps) <= done]
            launched = False
            for s in sorted(ready, key=lambda s: s.priority):
                if s.skip is not None:
                    reason = s.skip()
                    if reason:
                        echo(f"==> {s.label}: {reason}")
                        pending.remove(s)
                        done.add(s.name)
                        launched = True
                        continue
                if s.weight > 0 and used() + s.weight > budget and used() > 0:
                    continue  # 予算オーバー（ただし空のときは重量級でも1つは通す）
                pending.remove(s)
                # フラグ止まりにする区間は Popen〜登録の1起動ぶんだけに絞り、起動ごとに
                # キャンセルを確認する（ループ全体を覆うと、キャンセル後も同じ ready 集合の
                # 後続ステップを起動してしまう）
                in_launch = True
                launch(s)
                in_launch = False
                launched = True
                if cancelled:  # launch 中に届いていたぶんをここで拾う（子は登録済み）
                    raise SystemExit(143)
            if cancelled:
                raise SystemExit(143)
            if launched:
                announce()

            if not running:
                if launched:
                    continue  # skip で done が増えた直後は ready を再評価する
                if pending:
                    stuck = ", ".join(s.name for s in pending)
                    raise RuntimeError(f"依存が解決できないステップが残った: {stuck}")
                break

            # 完了を待つ
            time.sleep(0.1)
            finished = [r for r in running.values() if r.proc.poll() is not None]
            for r in finished:
                if r.proc.returncode != 0:
                    # running から消さずに投げる → except の stop_all が失敗ステップ自身の
                    # グループ（親の死後も残るワーカー）まで掃除する。
                    # ここで drain スレッドを join しない — 生き残りの子孫がパイプの
                    # write 端を握っていると EOF が来ず、kill 前の join は無駄に待つだけ
                    raise StepFailed(r)
                for t in r.threads:
                    t.join(timeout=5)
                del running[r.step.name]
                done.add(r.step.name)
                echo(f"==> 完了: {r.step.label}（{time.monotonic() - r.t0:.0f}秒）")
            if finished:
                announce()
    except BaseException:
        stop_all(list(running.values()))
        raise
    finally:
        signal.signal(signal.SIGTERM, prev_term)
        signal.signal(signal.SIGHUP, prev_hup)


def build_steps(ref: Path) -> list:
    py = sys.executable
    stems = ref / "stems"
    a = ref / "analysis"
    track = str(ref / "track.wav")
    bass4 = str(stems / "htdemucs" / "track" / "bass.wav")

    def demucs_skip(model: str):
        return lambda: "分離済み。スキップ" if (stems / model / "track").is_dir() else None

    def topline(label: str, stem_wav: str) -> Step:
        return Step(
            f"topline-{label}",
            f"上モノ({label})",
            [py, "topline.py", stem_wav, bass4, str(a / "basics.json"), str(a), "--label", label, "--mix", track],
            deps=("demucs-6s", "demucs-4s", "basics") if label.startswith("6s") else ("demucs-4s", "basics"),
            priority=40,
        )

    steps = [
        Step("overview", "全体像", [py, "overview.py", track, str(a / "overview.png"), "--title", ref.name],
             priority=60),
        # 2モデル走らせるのは実験の名残ではない。片方に絞ると分析が静かに劣化する。
        # htdemucs(4分割) と htdemucs_6s(6分割) は別々に訓練された別モデルで、
        # 共通のはずの drums/bass/vocals も出てくる音が違う。用途で使い分けている:
        #   ベース  → 4分割。調波打楽器比 10.4 に対し 6分割は 5.14 で、打楽器成分が倍入る
        #            （キック混入とみられる）。ピッチ追跡とオンセット位置が狂う
        #   上モノ  → 6分割。piano/guitar/other の3つに分かれるので、同じコード検出を
        #            3回かけて答えが揃うかを検算できる。4分割は上モノが other 1本で検算できず、
        #            実際にループ長を 8小節 と誤答した（4小節=0.898 vs 8小節=0.913 の僅差。
        #            6分割の3ステムは揃って4小節）
        # 詳細は README「なぜ4分割と6分割を両方使うのか」
        Step("demucs-4s", "ステム分離(4s)", [py, "-m", "demucs", "-d", "mps", "-n", "htdemucs", "-o", str(stems), track],
             weight=0, priority=10, skip=demucs_skip("htdemucs")),
        Step("demucs-6s", "ステム分離(6s)", [py, "-m", "demucs", "-d", "mps", "-n", "htdemucs_6s", "-o", str(stems), track],
             deps=("demucs-4s",), weight=0, priority=10, skip=demucs_skip("htdemucs_6s")),
        Step("basics", "BPM/キー/構成", [py, "basics.py", track, str(a)], priority=20),
        Step("stems-4s", "ステム表(4s)",
             [py, "stems.py", str(stems / "htdemucs" / "track"), track, str(a / "basics.json"), str(a),
              "--label", "4s", "--workers", "3"],
             deps=("demucs-4s", "basics"), weight=3, priority=30),
        Step("stems-6s", "ステム表(6s)",
             [py, "stems.py", str(stems / "htdemucs_6s" / "track"), track, str(a / "basics.json"), str(a),
              "--label", "6s", "--workers", "3"],
             deps=("demucs-6s", "basics"), weight=3, priority=25),
        Step("arrange", "構成マップ", [py, "arrange.py", str(a / "stems-6s.json"), str(a)],
             deps=("stems-6s",), priority=50),
        # グルーヴとコードのルート判定は4分割のベースを使う（上の理由）
        Step("groove", "グルーヴ", [py, "groove.py", str(stems / "htdemucs" / "track"), str(a / "basics.json"), str(a)],
             deps=("demucs-4s", "basics"), weight=2, priority=15),  # 内部でドラム帯域とベース(pyin)が並走
        topline("other", str(stems / "htdemucs" / "track" / "other.wav")),
        topline("6s-piano", str(stems / "htdemucs_6s" / "track" / "piano.wav")),
        topline("6s-guitar", str(stems / "htdemucs_6s" / "track" / "guitar.wav")),
        topline("6s-other", str(stems / "htdemucs_6s" / "track" / "other.wav")),
        Step("gates", "信頼性の判定", [py, "gates.py", str(a)],
             deps=("basics", "stems-4s", "stems-6s", "arrange", "groove",
                   "topline-other", "topline-6s-piano", "topline-6s-guitar", "topline-6s-other"),
             priority=70),
        Step("excerpts", "耳で確認するクリップ", [py, "excerpts.py", str(ref)], deps=("gates",), priority=70),
        Step("card", "制約カード", [py, "card.py", str(ref)], deps=("excerpts",), priority=70),
    ]
    for s in steps:
        # demucs（torch/MPS）は CPU をほぼ使わない（単独実測 0.3コア）が、torch の
        # スレッドプールは論理コア数ぶんスピンして並走ステップを轢く。2 で十分
        # （キャップなしだと basics が 35秒→73秒に轢かれる実測）。
        # なお taskpolicy -c utility で非クリティカルステップを E コアに寄せる案は
        # 実測で効果なし（むしろ悪化）だったので採用していない
        s.env = {**THREAD_CAP_ENV, "OMP_NUM_THREADS": "2"} if s.name.startswith("demucs") else THREAD_CAP_ENV
    return steps


def main() -> int:
    if len(sys.argv) != 2:
        print("使い方: analyze.py <リファレンスフォルダ>", file=sys.stderr)
        return 2
    ref = Path(sys.argv[1]).expanduser().resolve()
    if not (ref / "track.wav").is_file():
        print(f"ERROR: {ref}/track.wav がない（トリム済みの本編を置くこと）", file=sys.stderr)
        return 2
    (ref / "analysis").mkdir(exist_ok=True)

    # 旧カードは分析を始めた時点で無効。途中のステップで失敗したとき、
    # 更新済みの analysis/*.json の横に旧分析由来の card.json が残るのを防ぐ
    # （ref:card でいつでも再生成できるので消すのは安い）
    (ref / "card.json").unlink(missing_ok=True)

    try:
        run_dag(build_steps(ref))
    except StepFailed as e:
        r = e.running
        for line in list(r.stdout_tail)[-10:]:
            print(line, file=sys.stderr)
        for line in list(r.stderr_tail)[-20:]:
            print(line, file=sys.stderr)
        # ReferenceAnalyzer は stderr を末尾から走査して "error" を含む行を失敗理由に採用する
        # → 要約行を最後に置く
        detail = next((l for l in reversed(r.stderr_tail) if l.strip()), "")
        print(f"ERROR: 「{r.step.label}」が失敗 (exit {r.proc.returncode}): {detail}", file=sys.stderr)
        return 1

    print(f"\n完了: {ref / 'analysis'}", flush=True)
    print(f"次: open '{ref}/listen' を開いて、番号順にスペースキーで再生（{ref}/listen/README.md に聴きどころ）",
          flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
