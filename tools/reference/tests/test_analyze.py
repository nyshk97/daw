#!/usr/bin/env python
"""analyze.py（DAG オーケストレータ）の回帰テスト（プレーン assert・pytest 不要）。

実曲を使うと1周2分かかるので、回帰の固定はダミーステップの小さな DAG で行う。
固定する性質: 依存順序・キャッシュ skip・大量 stdout/stderr でも詰まらない・
失敗ステップの依存先を起動しない（fail-fast・子孫プロセスも停止）・CLI の非ゼロ終了。

使い方: .venv/bin/python tests/test_analyze.py
"""
import os
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from analyze import Step, StepFailed, run_dag  # noqa: E402

PY = sys.executable
checks = 0


def ok(cond: bool, msg: str) -> None:
    global checks
    assert cond, msg
    checks += 1


def stamp_cmd(path: Path, sleep: float = 0.0) -> list:
    """開始・終了時刻を <path>.start / <path>.end に記録するダミーステップ。"""
    code = (
        "import sys,time;"
        "open(sys.argv[1]+'.start','w').write(repr(time.time()));"
        f"time.sleep({sleep});"
        "open(sys.argv[1]+'.end','w').write(repr(time.time()))"
    )
    return [PY, "-c", code, str(path)]


def read_stamp(path: Path, kind: str) -> float:
    return float(Path(f"{path}.{kind}").read_text())


def test_dependency_order(tmp: Path) -> None:
    a, b = tmp / "a", tmp / "b"
    run_dag([
        Step("a", "A", stamp_cmd(a, sleep=0.3)),
        Step("b", "B", stamp_cmd(b), deps=("a",)),
    ])
    ok(read_stamp(b, "start") >= read_stamp(a, "end"), "依存先 B は A の終了後に起動する")


def test_cache_skip(tmp: Path) -> None:
    a, b = tmp / "skip-a", tmp / "skip-b"
    lines = []
    run_dag(
        [
            Step("a", "A", stamp_cmd(a), skip=lambda: "スキップ"),
            Step("b", "B", stamp_cmd(b), deps=("a",)),
        ],
        echo=lines.append,
    )
    ok(not Path(f"{a}.start").exists(), "skip したステップのコマンドは走らない")
    ok(Path(f"{b}.end").exists(), "skip は完了扱いで、依存先は走る")
    ok(any("スキップ" in l for l in lines), "skip の理由が進捗に出る")


def test_heavy_output(tmp: Path) -> None:
    # drain がないと PIPE バッファ満杯（64KB）でデッドロックする量を stdout/stderr 両方に書く。
    # drain が退行すると永久停止するので、直接 run_dag() せずサブプロセス＋timeout で囲む
    runner = (
        "import sys;"
        f"sys.path.insert(0, {str(Path(__file__).resolve().parent.parent)!r});"
        "from analyze import Step, run_dag;"
        "run_dag([Step('noisy', 'N', [sys.executable, '-c',"
        "  \"import sys; sys.stdout.write('o'*3_000_000); sys.stderr.write('e'*3_000_000)\"])]);"
        "print('done')"
    )
    r = subprocess.run([PY, "-c", runner], capture_output=True, text=True, timeout=60)
    ok(r.returncode == 0 and "done" in r.stdout, f"5MB 出力するステップでも完走する: {r.stderr[-300:]}")


def test_budget(tmp: Path) -> None:
    a, b = tmp / "bud-a", tmp / "bud-b"
    run_dag(
        [
            Step("a", "A", stamp_cmd(a, sleep=0.3), weight=1),
            Step("b", "B", stamp_cmd(b, sleep=0.3), weight=1),
        ],
        budget=1,
    )
    overlap = min(read_stamp(a, "end"), read_stamp(b, "end")) - max(read_stamp(a, "start"), read_stamp(b, "start"))
    ok(overlap <= 0.05, f"予算1では重み1のステップ同士が並走しない (overlap={overlap:.3f}s)")


# 停止漏れの2大パターンを再現するプロセスツリー生成スクリプト。
# ・failstep: 自分の子（sleeper）を残して exit 1 → 「失敗ステップ自身の子孫」の掃除を試す
# ・step→mid→leaf: mid は leaf を作ってすぐ死ぬ（reparent を意図的に起こす）。
#   leaf は SIGTERM を無視する → killpg の KILL エスカレーションでしか死なない
TREE_PY = """\
import os, signal, subprocess, sys, time
role, pidfile = sys.argv[1], sys.argv[2]
if role == "step":
    subprocess.Popen([sys.executable, __file__, "mid", pidfile])
    time.sleep(60)
elif role == "mid":
    subprocess.Popen([sys.executable, __file__, "leaf", pidfile])
    # すぐ exit → leaf は reparent される（ppid 追跡では見失う形）
elif role == "leaf":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    open(pidfile, "w").write(str(os.getpid()))
    time.sleep(60)
elif role == "sleeper":
    open(pidfile, "w").write(str(os.getpid()))
    time.sleep(60)
elif role == "failstep":
    subprocess.Popen([sys.executable, __file__, "sleeper", pidfile])
    time.sleep(0.8)
    sys.exit(1)
"""


def read_pid(path: Path, timeout: float = 5.0) -> int:
    t0 = time.time()
    while time.time() - t0 < timeout:
        if path.exists() and path.read_text():
            return int(path.read_text())
        time.sleep(0.05)
    raise AssertionError(f"pidfile が書かれない: {path}")


def wait_dead(pid: int, what: str, timeout: float = 5.0) -> None:
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            os.kill(pid, 0)
            time.sleep(0.1)
        except ProcessLookupError:
            return
    os.kill(pid, 9)  # テストを汚さないよう後始末してから落とす
    ok(False, f"{what}（pid {pid}）が停止していない")


def test_fail_fast(tmp: Path) -> None:
    tree = tmp / "tree.py"
    tree.write_text(TREE_PY)
    c = tmp / "ff-c"
    fail_child_pid = tmp / "fail-child.pid"
    leaf_pid = tmp / "leaf.pid"
    t0 = time.time()
    try:
        run_dag([
            # 失敗するステップ自身が子（sleeper）を残す
            Step("a", "A", [PY, str(tree), "failstep", str(fail_child_pid)]),
            # 別ステップは 子→孫 の2階層。孫は SIGTERM 無視・中間は即死（reparent）
            Step("b", "B", [PY, str(tree), "step", str(leaf_pid)]),
            Step("c", "C", stamp_cmd(c), deps=("a",)),
        ])
        ok(False, "失敗があれば StepFailed が飛ぶ")
    except StepFailed as e:
        ok(e.running.step.name == "a", "失敗したステップが特定される")
        ok(e.running.proc.returncode == 1, "exit code が保持される")
    ok(time.time() - t0 < 10, "実行中の B を待ち続けない（fail-fast）")
    ok(not Path(f"{c}.start").exists(), "失敗ステップの依存先 C は起動しない")
    wait_dead(read_pid(fail_child_pid), "失敗ステップ自身が残した子")
    ok(True, "失敗ステップ自身の子孫も停止する")
    wait_dead(read_pid(leaf_pid), "reparent された SIGTERM 無視の孫")
    ok(True, "reparent + SIGTERM 無視の孫も停止する（KILL エスカレーション）")


def test_nonfatal_step(tmp: Path) -> None:
    # fatal=False のステップ（キャッシュ温め等の任意ステップ）が失敗しても DAG は完走する。
    # 失敗ステップが子（sleeper）を残すパターンで、続行前にグループが掃除されることも固定する
    tree = tmp / "nf-tree.py"
    tree.write_text(TREE_PY)
    b = tmp / "nf-b"
    child_pid = tmp / "nf-child.pid"
    lines = []
    run_dag(
        [
            Step("warm", "温め", [PY, str(tree), "failstep", str(child_pid)], fatal=False),
            Step("b", "B", stamp_cmd(b)),
        ],
        echo=lines.append,
    )
    ok(Path(f"{b}.end").exists(), "非致命ステップが失敗しても他ステップは完走する")
    ok(any("失敗（続行）" in l for l in lines), "非致命の失敗が進捗に申告される")
    wait_dead(read_pid(child_pid), "非致命の失敗ステップが残した子")
    ok(True, "非致命の失敗ステップの子孫も続行前に停止する")

    # 非致命ステップへの依存は起動時に弾く（失敗時に出力が無いため設計として許さない）
    try:
        run_dag([
            Step("warm", "温め", [PY, "-c", "pass"], fatal=False),
            Step("b", "B", [PY, "-c", "pass"], deps=("warm",)),
        ])
        ok(False, "非致命ステップへの依存は ValueError")
    except ValueError:
        ok(True, "非致命ステップへの依存は ValueError")


def test_sigterm_during_launch(tmp: Path) -> None:
    # Popen（子グループ作成）から running 登録までの窓に SIGTERM が入っても、
    # ①その子が孤児化しない（ハンドラは launch 中フラグだけ立て、登録後に raise する）
    # ②同じ ready 集合の後続ステップをキャンセル後に起動しない、の両方を確認する
    real_popen = subprocess.Popen
    spawned = []

    def popen_hook(*args, **kwargs):
        p = real_popen(*args, **kwargs)
        # ステップの起動（process_group=0 付き）だけ数える。stop_all の pgrep も
        # subprocess.run → Popen でここを通るため、無条件に数えると誤検出する
        if kwargs.get("process_group") == 0:
            if not spawned:  # 最初の起動＝登録前の窓のど真ん中で撃つ
                os.kill(os.getpid(), signal.SIGTERM)
            spawned.append(p)
        return p

    subprocess.Popen = popen_hook
    try:
        try:
            run_dag([  # 独立な3ステップ＝同じ ready 集合
                Step("a", "A", [PY, "-c", "import time; time.sleep(60)"]),
                Step("b", "B", [PY, "-c", "import time; time.sleep(60)"]),
                Step("c", "C", [PY, "-c", "import time; time.sleep(60)"]),
            ])
            ok(False, "SIGTERM で SystemExit になる")
        except SystemExit as e:
            ok(e.code == 143, "SIGTERM は exit 143 に変換される")
    finally:
        subprocess.Popen = real_popen
    ok(len(spawned) == 1, f"キャンセル後は同じ ready 集合の後続を起動しない（起動数={len(spawned)}）")
    p = spawned[0]
    ok(p.poll() is not None, "起動直後（登録前）に SIGTERM を受けた子も回収される")
    r = subprocess.run(["pgrep", "-g", str(p.pid)], capture_output=True)
    ok(r.returncode != 0, "その子のプロセスグループも全滅している")


def test_dep_validation_restores_handlers(tmp: Path) -> None:
    # 依存の検証エラー（ハンドラ設定前に起きるべき）で SIGTERM/SIGHUP が置き換わったまま
    # 戻らない退行を防ぐ
    before_term = signal.getsignal(signal.SIGTERM)
    before_hup = signal.getsignal(signal.SIGHUP)
    try:
        run_dag([Step("a", "A", [PY, "-c", "pass"], deps=("missing",))])
        ok(False, "未知の依存は ValueError")
    except ValueError:
        pass
    ok(signal.getsignal(signal.SIGTERM) is before_term, "ValueError 後も SIGTERM ハンドラが元のまま")
    ok(signal.getsignal(signal.SIGHUP) is before_hup, "ValueError 後も SIGHUP ハンドラが元のまま")


def test_cli_nonzero_exit(tmp: Path) -> None:
    r = subprocess.run(
        [PY, str(Path(__file__).resolve().parent.parent / "analyze.py"), str(tmp / "no-such-ref")],
        capture_output=True,
        text=True,
    )
    ok(r.returncode != 0, "track.wav がないフォルダでは非ゼロ終了")
    ok("track.wav" in r.stderr, "理由が stderr に出る")


def main() -> None:
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        test_dependency_order(tmp)
        test_cache_skip(tmp)
        test_heavy_output(tmp)
        test_budget(tmp)
        test_fail_fast(tmp)
        test_nonfatal_step(tmp)
        test_sigterm_during_launch(tmp)
        test_dep_validation_restores_handlers(tmp)
        test_cli_nonzero_exit(tmp)
    print(f"OK: {checks} 件のチェックが通った")


if __name__ == "__main__":
    main()
