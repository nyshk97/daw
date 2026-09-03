#pragma once

#include <juce_core/juce_core.h>

// ステム分離の進捗（separate.sh の stderr 行から組み立てる。案C・2026-09-03確定モック）。
//
// 段階は separate.sh 自身が出す "salva-stage: <name>" 行で切り替え、段階内の進捗は
// demucs（tqdm）の " NN%|" 行から拾う。両方 stderr なので到着順が崩れない
// （stdout と stderr に分けると poll の同一周回で前後しうる）。
// demucs の文言（"Selected model is a bag of…"）は数えない: モデルを差し替えても
// スクリプトがマーカーを出す契約だけ守れば UI は変わらない。
//
// 段階の重みは実測（8秒の音源で2モデルがほぼ同時間・書き出し〔リサンプル＋24bit化〕は短い）
struct SeparationProgress
{
    enum class Stage { preparing, stems4, stems6, exporting };

    Stage stage = Stage::preparing;
    float local = 0.0f; // 段階内の進捗 0..1

    static constexpr float weightStems4 = 0.47f;
    static constexpr float weightStems6 = 0.47f;
    static constexpr float weightExport = 0.06f;

    // separate.sh が出すマーカー（stderr）。値は StemCache のグループ名ではなく段階名
    static constexpr const char* markerPrefix = "salva-stage: ";

    // 1行を解釈して状態を進める。戻り値 = 表示に影響する変化があったか
    bool consumeLine (const juce::String& rawLine)
    {
        const auto line = rawLine.trim();
        if (line.startsWith (markerPrefix))
        {
            const auto name = line.substring ((int) juce::CharPointer_UTF8 (markerPrefix).length()).trim();
            Stage next = stage;
            if (name == "4stems")       next = Stage::stems4;
            else if (name == "6stems")  next = Stage::stems6;
            else if (name == "export")  next = Stage::exporting;
            else
                return false; // 未知のマーカーは無視（将来の段階追加で旧アプリを壊さない）
            stage = next;
            local = 0.0f;
            return true;
        }

        // tqdm: " 50%|████      | 5.85/11.7 [00:00<00:00, 10.47seconds/s]"
        const int bar = line.indexOf ("%|");
        if (bar <= 0 || bar > 3)
            return false;
        const auto digits = line.substring (0, bar);
        if (! digits.containsOnly ("0123456789"))
            return false;
        const float pct = juce::jlimit (0.0f, 1.0f, (float) digits.getIntValue() / 100.0f);
        if (stage == Stage::preparing || stage == Stage::exporting)
            return false; // 段階外の進捗行（モデルのDL等）は表示に使わない
        if (pct <= local)
            return false; // 段階内は単調。同値（最終行の再描画・段階頭の0%）は変化なし
        local = pct;
        return true;
    }

    // 全体の進捗 0..1（書き出し段階は進捗行が無いので段階頭の値で止まる）
    float overall() const
    {
        switch (stage)
        {
            case Stage::preparing: return 0.0f;
            case Stage::stems4:    return weightStems4 * local;
            case Stage::stems6:    return weightStems4 + weightStems6 * local;
            case Stage::exporting: return weightStems4 + weightStems6;
        }
        return 0.0f;
    }

    // 残り時間の見積もり（秒）。経過÷進捗の素朴な外挿。進捗が小さいうち（モデル読み込み中）は
    // 外挿が暴れるので -1（未知）を返す
    static double estimateRemainingSeconds (double elapsedSeconds, float overallProgress)
    {
        if (overallProgress < 0.03f || elapsedSeconds <= 0.0)
            return -1.0;
        return juce::jmax (0.0, elapsedSeconds / (double) overallProgress - elapsedSeconds);
    }
};
