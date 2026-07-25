#pragma once

#include <juce_core/juce_core.h>

// オーディオブラウザの試聴の「いつ鳴らし・いつ止めるか」だけを持つ判定。
// 描画・タイマー・再生の実行は AudioFileBrowserView 側が Result を見て行う
// （このヘッダはGUI非依存にして daw_tests から直接テストする）。
class PreviewPolicy
{
public:
    // 試聴の出自。トランスポート開始・オートプレビューOFFで止めるのは auto 由来だけ
    enum class Origin { none, autoSelect, manual };

    // 呼び出し側が実行すべき副作用
    struct Result
    {
        bool startTimer = false;   // デバウンスタイマーを開始する
        bool stopTimer = false;    // デバウンスタイマーを止める
        bool startPreview = false; // startFile の試聴を始める
        bool stopPreview = false;  // 試聴を止める
        juce::File startFile;
        juce::Array<juce::File> repaint; // 表示（♫/▶/■）が変わった行
    };

    bool isEnabled() const { return enabled; }
    bool isTransportRunning() const { return transportRunning; }

    // ■ を出す行か。loading 中も試聴対象として扱う（デコード中にも停止できるように）
    bool isActive (const juce::File& file) const
    {
        return activeFile != juce::File() && activeFile == file;
    }

    juce::File pending() const { return pendingFile; }
    juce::File active() const { return activeFile; }

    // パネルを閉じる・取り込みを始めるなど、外から試聴を畳む。出自は問わない
    Result cancelAll()
    {
        Result result;
        cancelPending (result);
        clearActive (result, true);
        // 止める対象が無くても停止要求は出す（前回の失敗で出たエラー表示もここで消える）
        result.stopPreview = true;
        return result;
    }

    // 行選択が変わった。playable でない（フォルダ・非対応・インポート中）ときは予約しない
    Result selectionChanged (const juce::File& file, bool playable)
    {
        Result result = cancelAll(); // 別の行を選ぶ＝停止（出自を問わない）
        if (enabled && ! transportRunning && playable && file != juce::File())
        {
            pendingFile = file;
            pendingOrigin = Origin::autoSelect;
            result.startTimer = true;
        }
        return result;
    }

    // デバウンス満了。予約があれば試聴を始める
    Result takePending()
    {
        Result result;
        if (pendingFile == juce::File())
            return result;
        setActive (pendingFile, pendingOrigin, result);
        result.startPreview = true;
        result.startFile = pendingFile;
        pendingFile = juce::File();
        pendingOrigin = Origin::none;
        return result;
    }

    // 走行中フラグは毎フレーム渡されるので、停止→走行の遷移時だけ処理する
    Result setTransportRunning (bool running)
    {
        Result result;
        if (running == transportRunning)
            return result;
        transportRunning = running;
        if (running)
            stopAutoOnly (result);
        return result;
    }

    Result setEnabled (bool on)
    {
        Result result;
        if (on == enabled)
            return result;
        enabled = on;
        if (! on)
            stopAutoOnly (result);
        return result;
    }

    // 行アイコンのクリック。同じファイルなら停止、それ以外は manual として即開始する
    Result iconClicked (const juce::File& file)
    {
        Result result;
        cancelPending (result);
        if (isActive (file))
        {
            clearActive (result, true);
            return result;
        }
        clearActive (result, false); // 前の行の■を消す（start() が上書きするので停止要求は出さない）
        setActive (file, Origin::manual, result);
        result.startPreview = true;
        result.startFile = file;
        return result;
    }

    // 毎フレームの試聴状態。末尾到達・失敗で idle に戻ったら試聴対象を解除する
    // （AudioFilePreview はオーディオスレッドから自分で idle に戻るため、これを怠ると■が残る）
    Result previewStateChanged (bool loading, bool playing)
    {
        Result result;
        if (! loading && ! playing)
            clearActive (result, false);
        return result;
    }

private:
    void setActive (const juce::File& file, Origin origin, Result& result)
    {
        activeFile = file;
        activeOrigin = origin;
        result.repaint.addIfNotAlreadyThere (file);
    }

    void clearActive (Result& result, bool alsoStop)
    {
        if (activeFile == juce::File())
            return;
        result.repaint.addIfNotAlreadyThere (activeFile);
        if (alsoStop)
            result.stopPreview = true;
        activeFile = juce::File();
        activeOrigin = Origin::none;
    }

    void cancelPending (Result& result)
    {
        if (pendingFile == juce::File())
            return;
        pendingFile = juce::File();
        pendingOrigin = Origin::none;
        result.stopTimer = true;
    }

    // トランスポート開始・トグルOFFで止めるのは auto 由来だけ（手動▶は残す）
    void stopAutoOnly (Result& result)
    {
        if (pendingOrigin == Origin::autoSelect)
            cancelPending (result);
        if (activeOrigin == Origin::autoSelect)
            clearActive (result, true);
    }

    bool enabled = true; // セッション内のみ保持。起動時は常にON
    bool transportRunning = false;
    juce::File pendingFile;
    Origin pendingOrigin = Origin::none;
    juce::File activeFile;
    Origin activeOrigin = Origin::none;
};
