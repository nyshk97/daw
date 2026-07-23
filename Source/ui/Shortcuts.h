#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// ショートカットキー定義の単一の真実の源。
//
// 運用ルール（追加・変更時に必ず守る）:
// - ショートカットを追加するときはこのテーブルに1行足し、
//   MainComponent::keyPressed では matches() で判定する（KeyPress直書き禁止）
// - ツールチップ・右クリックメニュー・⌘?一覧の表記は keyText()/tooltipText() から
//   生成する（表記文字列の手書き禁止。⌘?一覧はテーブル走査で自動生成される）
//
// 表示（keyLabel）と入力判定（matcher）は意図的に分離している。
// ,/. のレイアウト依存（Shift+,/. が <> の文字で届く）・Delete/Backspace の
// 2キーコード対応・修飾なし1文字キーの「⌘/⌃/⌥が押されていない」ガードなど、
// 単一の juce::KeyPress の等値比較では表せない判定があるため。
namespace Shortcuts
{

enum class ID
{
    // トランスポート
    playStop,
    record,
    toggleCycle,
    seekBeat,
    seekBar,
    seekSection,
    // 編集
    undo,
    redo,
    split,
    muteRegion,
    deleteItem,
    exportRegion,
    // トラック
    addAudioTrack,
    addMidiTrack,
    deleteTrack,
    muteTrack,
    toggleSolo,
    // ピアノロール（表示中のみ有効。有効条件はkeyPressed側が持つ）
    noteSemitone,
    noteOctave,
    noteCopy,
    notePaste,
    // 表示・ズーム
    zoomHorizontal,
    toggleMixer,
    toggleFxEditor,
    shortcutList,
    // プロジェクト
    save,
    bounce,
    openChooser,
    audioSettings,
};

enum class Category { transport, editing, track, pianoRoll, view, project };

struct Entry
{
    ID id;
    Category category;
    const char* name;     // u8リテラル。UIへ渡すときは必ずfromUTF8を通す（下のヘルパー経由）
    const char* keyLabel; // u8リテラル。Mac記号表記（⌘⇧⌥⌃）で固定。レイアウト依存キーが
                          // あるためgetTextDescriptionWithIcons等の自動生成は使わない
    bool (*matcher) (const juce::KeyPress&);

    // ネイティブメニュー（NSMenuのkeyEquivalent）に載せる項目だけが持つKeyPress。
    // JUCEはApplicationCommandManagerのKeyPressMappings経由でしかkeyEquivalentを
    // 設定しないため、matcherとは別にKeyPressオブジェクトが要る（単修飾＋1文字の
    // 単純なキーのみ載せる前提。レイアウト依存キーはメニューに載せない）
    juce::KeyPress menuKey {};
};

namespace detail
{
// 修飾なし1文字ショートカット（r/m/,/.）用: ⌘/⌃/⌥が押されていないこと（Shiftは許容）
inline bool noCmdCtrlAlt (const juce::KeyPress& k)
{
    return ! k.getModifiers().testFlags (juce::ModifierKeys::commandModifier
                                         | juce::ModifierKeys::ctrlModifier
                                         | juce::ModifierKeys::altModifier);
}

inline bool isDeleteOrBackspace (const juce::KeyPress& k)
{
    return k.getKeyCode() == juce::KeyPress::deleteKey
        || k.getKeyCode() == juce::KeyPress::backspaceKey;
}
} // namespace detail

// テーブル順 = ⌘?一覧での表示順（カテゴリ内）
inline const Entry table[] = {
    // ---- トランスポート ----
    { ID::playStop, Category::transport, u8"再生/停止", u8"Space",
      [] (const juce::KeyPress& k) { return k == juce::KeyPress::spaceKey; } },
    { ID::record, Category::transport, u8"録音", u8"R",
      [] (const juce::KeyPress& k)
      { return detail::noCmdCtrlAlt (k) && k.getTextCharacter() == 'r'; } },
    // Logic準拠: C = サイクル（ループ範囲）の入/切。範囲はルーラーのドラッグで作る
    { ID::toggleCycle, Category::transport, u8"サイクル（ループ範囲）入/切", u8"C",
      [] (const juce::KeyPress& k)
      { return detail::noCmdCtrlAlt (k) && k.getTextCharacter() == 'c'; } },
    { ID::seekBeat, Category::transport, u8"1拍シーク", u8", / .",
      [] (const juce::KeyPress& k)
      {
          const auto tc = k.getTextCharacter();
          return detail::noCmdCtrlAlt (k) && ! k.getModifiers().isShiftDown()
              && (tc == ',' || tc == '.');
      } },
    { ID::seekBar, Category::transport, u8"1小節シーク", u8"< / >",
      [] (const juce::KeyPress& k)
      {
          // Shift+,/. はキーボードレイアウトによって文字が <> に変換されて届く
          const auto tc = k.getTextCharacter();
          return detail::noCmdCtrlAlt (k)
              && (tc == '<' || tc == '>'
                  || (k.getModifiers().isShiftDown() && (tc == ',' || tc == '.')));
      } },
    { ID::seekSection, Category::transport, u8"前/次のセクションへシーク", u8"⌥, / ⌥.",
      [] (const juce::KeyPress& k)
      {
          // ⌥でtextCharacterは記号（US配列で≤/≥等）に化けるためkeyCodeで判定する
          // （macのkeyCodeはcharactersIgnoringModifiers由来でShift以外の修飾を無視する）
          return k.getModifiers().isAltDown()
              && ! k.getModifiers().testFlags (juce::ModifierKeys::commandModifier
                                               | juce::ModifierKeys::ctrlModifier)
              && (k.getKeyCode() == ',' || k.getKeyCode() == '.');
      } },

    // ---- 編集 ----
    { ID::undo, Category::editing, u8"取り消す", u8"⌘Z",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0); } },
    { ID::redo, Category::editing, u8"やり直す", u8"⇧⌘Z",
      [] (const juce::KeyPress& k)
      {
          return k == juce::KeyPress ('z', juce::ModifierKeys::commandModifier
                                               | juce::ModifierKeys::shiftModifier, 0);
      } },
    { ID::split, Category::editing, u8"再生ヘッド位置で分割", u8"⌘T",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('t', juce::ModifierKeys::commandModifier, 0); } },
    { ID::muteRegion, Category::editing, u8"リージョン/クリップをミュート", u8"⌃M",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('m', juce::ModifierKeys::ctrlModifier, 0); } },
    { ID::deleteItem, Category::editing, u8"クリップ/リージョンを削除", u8"Delete",
      [] (const juce::KeyPress& k)
      {
          return detail::isDeleteOrBackspace (k)
              && ! k.getModifiers().testFlags (juce::ModifierKeys::commandModifier);
      } },
    // Logic準拠: ⌘E = 選択中のリージョン/クリップをオーディオファイルとして書き出す
    { ID::exportRegion, Category::editing, u8"リージョン/クリップを書き出す", u8"⌘E",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('e', juce::ModifierKeys::commandModifier, 0); } },

    // ---- トラック ----
    { ID::addAudioTrack, Category::track, u8"オーディオトラックを追加", u8"⌘⌥A",
      [] (const juce::KeyPress& k)
      {
          return k == juce::KeyPress ('a', juce::ModifierKeys::commandModifier
                                               | juce::ModifierKeys::altModifier, 0);
      } },
    { ID::addMidiTrack, Category::track, u8"ソフトウェア音源トラックを追加", u8"⌘⌥S",
      [] (const juce::KeyPress& k)
      {
          return k == juce::KeyPress ('s', juce::ModifierKeys::commandModifier
                                               | juce::ModifierKeys::altModifier, 0);
      } },
    { ID::deleteTrack, Category::track, u8"選択トラックを削除", u8"⌘Delete",
      [] (const juce::KeyPress& k)
      {
          return detail::isDeleteOrBackspace (k)
              && k.getModifiers().testFlags (juce::ModifierKeys::commandModifier);
      } },
    { ID::muteTrack, Category::track, u8"選択トラックをミュート", u8"M",
      [] (const juce::KeyPress& k)
      { return detail::noCmdCtrlAlt (k) && k.getTextCharacter() == 'm'; } },
    // Logic準拠: ソロ中なら全解除、ソロなしなら直近のソロ構成を再適用（無ければ選択トラック）
    { ID::toggleSolo, Category::track, u8"ソロを入/切（直近のソロ構成）", u8"S",
      [] (const juce::KeyPress& k)
      { return detail::noCmdCtrlAlt (k) && k.getTextCharacter() == 's'; } },

    // ---- ピアノロール ----
    { ID::noteSemitone, Category::pianoRoll, u8"ノートを半音上/下", u8"↑ / ↓",
      [] (const juce::KeyPress& k)
      {
          return k == juce::KeyPress (juce::KeyPress::upKey)
              || k == juce::KeyPress (juce::KeyPress::downKey);
      } },
    { ID::noteOctave, Category::pianoRoll, u8"ノートをオクターブ上/下", u8"⌥↑ / ⌥↓",
      [] (const juce::KeyPress& k)
      {
          return k == juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::altModifier, 0)
              || k == juce::KeyPress (juce::KeyPress::downKey, juce::ModifierKeys::altModifier, 0);
      } },
    { ID::noteCopy, Category::pianoRoll, u8"ノートをコピー", u8"⌘C",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('c', juce::ModifierKeys::commandModifier, 0); } },
    { ID::notePaste, Category::pianoRoll, u8"ノートをペースト", u8"⌘V",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('v', juce::ModifierKeys::commandModifier, 0); } },

    // ---- 表示・ズーム ----
    { ID::zoomHorizontal, Category::view, u8"横ズームアウト/イン", u8"⌘← / ⌘→",
      [] (const juce::KeyPress& k)
      {
          return k == juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::commandModifier, 0)
              || k == juce::KeyPress (juce::KeyPress::rightKey, juce::ModifierKeys::commandModifier, 0);
      } },
    // Logic準拠: X = ミキサー
    { ID::toggleMixer, Category::view, u8"ミキサーを表示/隠す", u8"X",
      [] (const juce::KeyPress& k)
      { return detail::noCmdCtrlAlt (k) && k.getTextCharacter() == 'x'; } },
    // Logic準拠: I = インスペクタ（本アプリでは左のFXパネル）
    { ID::toggleFxEditor, Category::view, u8"FXパネルを表示/隠す", u8"I",
      [] (const juce::KeyPress& k)
      { return detail::noCmdCtrlAlt (k) && k.getTextCharacter() == 'i'; } },
    { ID::shortcutList, Category::view, u8"ショートカット一覧", u8"⌘?",
      [] (const juce::KeyPress& k)
      {
          // ? は Shift+/ なのでShiftの有無は問わない（JIS/US両配列とも / キー）
          const auto tc = k.getTextCharacter();
          return k.getModifiers().testFlags (juce::ModifierKeys::commandModifier)
              && ! k.getModifiers().testFlags (juce::ModifierKeys::ctrlModifier
                                               | juce::ModifierKeys::altModifier)
              && (tc == '/' || tc == '?' || k.getKeyCode() == '/');
      } },

    // ---- プロジェクト ----
    { ID::save, Category::project, u8"保存", u8"⌘S",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0); },
      juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0) },
    { ID::bounce, Category::project, u8"書き出し", u8"⌘B",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0); },
      juce::KeyPress ('b', juce::ModifierKeys::commandModifier, 0) },
    { ID::openChooser, Category::project, u8"プロジェクトを閉じて選択画面へ", u8"⌘W / ⌘O",
      [] (const juce::KeyPress& k)
      {
          return k == juce::KeyPress ('w', juce::ModifierKeys::commandModifier, 0)
              || k == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0);
      },
      juce::KeyPress ('w', juce::ModifierKeys::commandModifier, 0) },
    { ID::audioSettings, Category::project, u8"オーディオ設定", u8"⌘,",
      [] (const juce::KeyPress& k)
      { return k == juce::KeyPress (',', juce::ModifierKeys::commandModifier, 0); } },
};

inline const Entry& entry (ID id)
{
    for (const auto& e : table)
        if (e.id == id)
            return e;
    jassertfalse; // テーブルに未登録のID
    return table[0];
}

inline bool matches (const juce::KeyPress& key, ID id)
{
    return entry (id).matcher (key);
}

inline juce::String name (ID id)     { return juce::String::fromUTF8 (entry (id).name); }
inline juce::String keyText (ID id)  { return juce::String::fromUTF8 (entry (id).keyLabel); }
inline juce::KeyPress menuKey (ID id) { return entry (id).menuKey; } // メニュー非掲載項目は無効なKeyPress

// ボタンのホバーツールチップ用: 「録音 (R)」形式
inline juce::String tooltipText (ID id)
{
    return name (id) + " (" + keyText (id) + ")";
}

inline juce::String categoryName (Category c)
{
    switch (c)
    {
        case Category::transport: return juce::String::fromUTF8 (u8"トランスポート");
        case Category::editing:   return juce::String::fromUTF8 (u8"編集");
        case Category::track:     return juce::String::fromUTF8 (u8"トラック");
        case Category::pianoRoll: return juce::String::fromUTF8 (u8"ピアノロール");
        case Category::view:      return juce::String::fromUTF8 (u8"表示・ズーム");
        case Category::project:   return juce::String::fromUTF8 (u8"プロジェクト");
    }
    return {};
}

} // namespace Shortcuts
