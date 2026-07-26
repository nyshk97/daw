#pragma once

#include <atomic>
#include <juce_audio_formats/juce_audio_formats.h>

// 外部オーディオファイルの取り込み（変換コピー）。オーディオデバイス・リアルタイムスレッドとは
// 無関係で、専用のバックグラウンドスレッド（juce::Thread）がデコード→リサンプル→24bit WAV
// 書き込みの全てを行う。BounceRendererと同じpull型（呼び出し側のTimerがstatus()をポーリング）。
//
// 変換仕様（docs/plans/2026-07-24-audio-import-stereo.md で確定）:
// - chの採用: 1chはモノ、2ch以上は先頭2chをL/Rとする（余剰chは捨てる）
// - 出力フレーム数 = llround(inputFrames × targetSR / sourceSR)（整数丸めで一意）
// - 補間器（WindowedSinc・200タップ）は各chで独立に状態保持
// - 入力終端は補間器へゼロを供給して末尾サンプルの欠落を防ぐ（processのwrapAround=0）
// - SRが一致する場合はリサンプルをバイパス（デコードのみ）
//
// 出力は一時ファイル（Project::save() のGCパターン clip-*.wav に掛からない名前）へ書く。
// 最終名 clip-NNN.wav へのリネームは呼び出し側がメッセージスレッドで行う
// （nextClipFile()は予約できないため、録音完了とのclip-NNN採番競合をリネーム時採番で防ぐ）
class AudioImporter : private juce::Thread
{
public:
    // 取り込み上限（デコードを全量メモリに載せるための安全弁。48kHzで約34分）。
    // クリップは再生時もメモリ常駐なので、これを超える素材はそもそも扱えない
    static constexpr juce::int64 maxInputFrames = 100'000'000;

    struct Request
    {
        juce::File sourceFile;         // 取り込み元（WAV/AIFF/FLAC/MP3/M4A等）
        juce::File tempFile;           // 一時出力先（呼び出し側がプロジェクトフォルダ内に採る）
        // プロジェクトSR。<= 0 は「元のSRを保つ」（リサンプルをバイパスして出力長も入力と同じにする）。
        // サンプル音源はデバイスSR変更に追従できるよう元SRのまま保存するため、この経路を使う
        double targetSampleRate = 0.0;
    };

    enum class Status { idle, running, success, cancelled, failed };

    struct Result
    {
        Status status = Status::idle;
        juce::String errorMessage;      // failed のとき
        juce::int64 outputFrames = 0;   // 変換後の長さ
        int numChannels = 0;            // 1=モノ / 2=ステレオ
        double sourceSampleRate = 0.0;  // ログ用
    };

    AudioImporter() : juce::Thread ("Audio Importer") {}

    ~AudioImporter() override
    {
        cancelAndWait();
    }

    // 実行中なら false。開始後、Request の所有はワーカーに移る
    bool start (Request&& requestToRun)
    {
        if (isThreadRunning())
            return false;
        request = std::move (requestToRun);
        result = {};
        progressValue.store (0.0f);
        currentStatus.store (Status::running);
        startThread();
        return true;
    }

    // 非同期キャンセル要求（ワーカーは次のチャンク境界で中断し、一時ファイルを削除して終了する）
    void cancel() { signalThreadShouldExit(); }

    // キャンセル要求＋ワーカー終了まで待つ（閉じる/プロジェクト切替フロー用。数秒想定）。
    // ワーカーは中断時に一時ファイルを削除してから戻る
    void cancelAndWait()
    {
        signalThreadShouldExit();
        stopThread (-1);
    }

    Status status() const { return currentStatus.load(); }
    float progress() const { return progressValue.load(); }

    // 終了後（status != running）に結果を取り出して idle に戻す
    Result takeResult()
    {
        jassert (! isThreadRunning());
        auto taken = std::move (result);
        result = {};
        currentStatus.store (Status::idle);
        return taken;
    }

private:
    void run() override;
    Status decodeAndWrite();

    Request request;
    Result result;
    std::atomic<Status> currentStatus { Status::idle };
    std::atomic<float> progressValue { 0.0f };

    JUCE_DECLARE_NON_COPYABLE (AudioImporter)
};
