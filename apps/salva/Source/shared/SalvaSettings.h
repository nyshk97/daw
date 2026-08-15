#pragma once

#include <juce_core/juce_core.h>

// 設定の永続化（~/Library/Application Support/salva/settings.json）。
// dev/release で共有する（LaLaのプロジェクト置き場共有と同じ考え方）。
// 保存はフィールドを丸ごと書き出す単純な形（設定画面はなく、操作の副作用として都度saveする）
struct SalvaSettings
{
    juce::String outputDeviceName;          // 出力デバイス（前回記憶）
    juce::String inputDeviceName;           // 入力デバイス（Phase 4）
    int inputChannelPairStart = 2;          // 入力ステレオペアの先頭ch（0-based。既定2 = 3-4ch = mk5背面Line In 1-2想定・実測後に見直し）
    juce::StringArray recentFiles;          // 最近開いたファイル（新しい順・最大8件）
    juce::String recordDirectory;           // 録音の保存先（空=既定 ~/Music/salva。設定JSONの手編集で上書き）
    juce::String exportDirectory;           // 区間書き出し先（Phase 5）
    juce::String venvPathOverride;          // ステム分離venvの上書き（Phase 5。空=既定）

    static constexpr int maxRecentFiles = 8;

    static juce::File defaultFile();
    static SalvaSettings load (const juce::File& file = defaultFile());
    void save (const juce::File& file = defaultFile()) const;

    // 先頭挿入・重複除去・maxRecentFiles件まで。存在しないパスの掃除は表示側で行う
    // （外部ドライブ上のファイルが「未マウント中に消える」のを避ける）
    void addRecentFile (const juce::String& path);
};
