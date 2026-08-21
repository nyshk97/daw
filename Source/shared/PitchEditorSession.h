#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "PitchCorrection.h"
#include "PitchCurve.h"
#include "PitchNotes.h"

// ピッチエディタの「対象と状態」（UI を含まない判断ロジック。daw_tests で固定する）。
// 設計の真実の源: docs/plans/2026-08-20-2244-vocal-pitch-correction.md（Phase 3）
//
// 開いたときの2経路:
// - 補正なしのクリップ: 解析 → 自動スナップを**初回プレビュー**（initialPreview）として開始。
//   「補正を有効化」または最初の手編集で確定（commitInitial）。閉じる・対象切替で捨てる
// - 補正ありのクリップ: 保存済みの状態を**確定状態のまま**表示（committed）。自動スナップしない
// 確定状態からの「キーに合わせ直す」「再解析」は**変更プレビュー**（changePreview）: Apply で確定、
// Cancel / 閉じる / 切替 / ⌘B / ⌘E で旧状態へ戻る。この間はノート操作・つまみを無効化
// （プレビューは自動結果のみ＝手作業を含まない）。
// サイドカーが書けていない間（sidecarBlocked）は編集を無効化する（確定の前提条件 = サイドカー存在）。
class PitchEditorSession
{
public:
    enum class Mode { closed, analyzing, initialPreview, committed, changePreview };

    Mode mode() const { return currentMode; }
    juce::uint64 clipId() const { return targetClipId; }
    juce::uint64 generation() const { return currentGeneration; } // 破棄・切替のたびに進む（遅着の結果を無視する）
    bool isOpen() const { return currentMode != Mode::closed; }
    bool sidecarBlocked() const { return blocked; }

    // 表示・プレビューに使う現在の状態（analyzing/closed では無効）
    const PitchCorrection& working() const { return workingState; }
    PitchCorrection& mutableWorking() { return workingState; }
    const std::shared_ptr<const PitchCurve>& curve() const { return workingCurve; }
    const std::vector<DetectedPitchNote>& detected() const { return detectedNotes; }

    // 手編集・つまみ・「補正を有効化」ができる状態か
    bool editable() const
    {
        return ! blocked && (currentMode == Mode::initialPreview || currentMode == Mode::committed);
    }
    // 「鳴らすべき音」がプレビュー（一時マップ）か、永続モデルか
    bool hasPreview() const { return currentMode == Mode::initialPreview || currentMode == Mode::changePreview; }

    // ---- 遷移 ----
    // 補正なしのクリップを開く（解析待ち）。generation を進める
    void openForAnalysis (juce::uint64 clipId)
    {
        close();
        targetClipId = clipId;
        currentMode = Mode::analyzing;
        ++currentGeneration;
    }
    // 補正ありのクリップを開く（確定状態のまま）
    void openCommitted (juce::uint64 clipId, const PitchCorrection& committed, std::shared_ptr<const PitchCurve> curve)
    {
        close();
        targetClipId = clipId;
        currentMode = Mode::committed;
        workingState = committed;
        workingCurve = std::move (curve);
        detectedNotes = workingCurve != nullptr ? PitchNotes::detect (*workingCurve) : std::vector<DetectedPitchNote>{};
        ++currentGeneration;
    }
    // 解析完了（analyzing → initialPreview）。autoSnap の結果を working に。sidecarWritten=false なら編集不可
    void analysisFinished (std::shared_ptr<const PitchCurve> curve, juce::int64 domainOffset, juce::int64 domainLength,
                           const std::optional<ProjectKey>& projectKey, bool sidecarWritten)
    {
        if (currentMode != Mode::analyzing)
            return;
        workingCurve = std::move (curve);
        detectedNotes = PitchNotes::detect (*workingCurve);
        workingState = PitchCorrections::autoSnap (*workingCurve, detectedNotes, domainOffset, domainLength,
                                                  projectKey, PitchScaleMode::projectKey, {});
        blocked = ! sidecarWritten;
        currentMode = Mode::initialPreview;
    }
    void setSidecarWritten (bool written) { blocked = ! written; }
    // 初回プレビューの確定（「補正を有効化」または最初の手編集）。戻り値: 確定した状態
    std::optional<PitchCorrection> commitInitial()
    {
        if (currentMode != Mode::initialPreview || blocked)
            return std::nullopt;
        currentMode = Mode::committed;
        return workingState;
    }
    // 変更プレビューの開始（キーに合わせ直す・再解析の結果を working に）。committed からのみ
    bool beginChangePreview (const PitchCorrection& proposal, std::shared_ptr<const PitchCurve> proposalCurve)
    {
        if (currentMode != Mode::committed)
            return false;
        backupState = workingState;
        backupCurve = workingCurve;
        workingState = proposal;
        workingCurve = std::move (proposalCurve);
        detectedNotes = workingCurve != nullptr ? PitchNotes::detect (*workingCurve) : std::vector<DetectedPitchNote>{};
        currentMode = Mode::changePreview;
        return true;
    }
    std::optional<PitchCorrection> applyChange()
    {
        if (currentMode != Mode::changePreview)
            return std::nullopt;
        currentMode = Mode::committed;
        backupState.reset();
        backupCurve = nullptr;
        return workingState;
    }
    bool cancelChange()
    {
        if (currentMode != Mode::changePreview)
            return false;
        workingState = backupState.value_or (workingState);
        workingCurve = backupCurve != nullptr ? backupCurve : workingCurve;
        detectedNotes = workingCurve != nullptr ? PitchNotes::detect (*workingCurve) : std::vector<DetectedPitchNote>{};
        backupState.reset();
        backupCurve = nullptr;
        currentMode = Mode::committed;
        ++currentGeneration;
        return true;
    }
    // 閉じる・対象切替・対象消失。generation を進めて遅着を無視する
    void close()
    {
        currentMode = Mode::closed;
        targetClipId = 0;
        workingState = {};
        workingCurve = nullptr;
        backupState.reset();
        backupCurve = nullptr;
        detectedNotes.clear();
        blocked = false;
        ++currentGeneration;
    }
    // undo 等で永続モデルが変わったときの再同期（committed のときだけ。working を永続値で置き換える）
    void syncCommitted (const PitchCorrection& committed, std::shared_ptr<const PitchCurve> curve)
    {
        if (currentMode != Mode::committed)
            return;
        workingState = committed;
        if (curve != workingCurve)
        {
            workingCurve = std::move (curve);
            detectedNotes = workingCurve != nullptr ? PitchNotes::detect (*workingCurve) : std::vector<DetectedPitchNote>{};
        }
    }

private:
    Mode currentMode = Mode::closed;
    juce::uint64 targetClipId = 0;
    juce::uint64 currentGeneration = 0;
    bool blocked = false;
    PitchCorrection workingState;
    std::shared_ptr<const PitchCurve> workingCurve;
    std::optional<PitchCorrection> backupState;
    std::shared_ptr<const PitchCurve> backupCurve;
    std::vector<DetectedPitchNote> detectedNotes;
};
