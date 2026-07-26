#pragma once

#include <functional>
#include <vector>
#include <juce_core/juce_core.h>

// 下部エリア（ピアノロール／FX詳細）で「何を見ていたか」の履歴。ブラウザ型
// （戻った状態で新しいものを開くと、進む側は破棄される）。UIスレッド専用で、
// セッション内のみ保持する（project.json には書かない）。
//
// カーソル操作は必ず
//     findValid() で非破壊に候補位置を探す → entryAt() を復元してみる → 成功したら commit()
// の順で使う。先にカーソルを動かすと、復元に失敗したとき「表示は前のまま・カーソルだけ進む」
// というズレが残るため、破壊的な「1つ進める」APIは意図的に用意していない。
class BottomPanelHistory
{
public:
    struct Entry
    {
        enum class Kind { pianoRoll, fxDetail };

        Kind kind = Kind::pianoRoll;
        // pianoRoll: 表示していたリージョン。fxDetail: channelKey=="track" のときの対象トラック
        juce::uint64 trackId = 0;
        juce::uint64 regionId = 0;  // pianoRoll のみ
        juce::String channelKey;    // fxDetail のみ: "track" / "bus0".."bus2" / "master"
        int slot = -1;              // fxDetail のみ（FXパネルのスロット番号）

        bool operator== (const Entry& other) const
        {
            if (kind != other.kind)
                return false;
            if (kind == Kind::pianoRoll)
                return trackId == other.trackId && regionId == other.regionId;
            return channelKey == other.channelKey && trackId == other.trackId && slot == other.slot;
        }
        bool operator!= (const Entry& other) const { return ! (*this == other); }
    };

    static constexpr int maxEntries = 16;

    // ユーザーが新しく開いたものを積む。現在位置より先は破棄する。
    // 現在位置と同じものなら何もしない（同じものを開き直しても履歴は伸びない）
    void push (const Entry& entry)
    {
        if (hasCurrent() && entries[(size_t) index] == entry)
            return;

        if (index + 1 < (int) entries.size())
            entries.erase (entries.begin() + index + 1, entries.end());

        entries.push_back (entry);

        if ((int) entries.size() > maxEntries)
            entries.erase (entries.begin()); // 古い方から捨てる

        index = (int) entries.size() - 1;
    }

    // カーソルを動かさず、現在エントリの内容だけを差し替える。
    // トラック選択への追従で下部の表示対象が変わったとき用（追従はユーザーが開く操作をした
    // わけではないので履歴を1件消費させない。かつ現在地と実表示は一致させる必要がある）
    void replaceCurrent (const Entry& entry)
    {
        if (hasCurrent())
            entries[(size_t) index] = entry;
    }

    bool hasCurrent() const               { return isValidPosition (index); }
    const Entry& current() const          { return entries[(size_t) index]; }
    int currentPosition() const           { return index; }

    bool isValidPosition (int position) const
    {
        return position >= 0 && position < (int) entries.size();
    }
    const Entry& entryAt (int position) const { return entries[(size_t) position]; }

    // direction 方向（-1 = 戻る / +1 = 進む）で最初に predicate を満たす位置を返す。
    // カーソルは動かさない。見つからなければ -1
    int findValid (int direction, const std::function<bool (const Entry&)>& predicate) const
    {
        if (direction == 0)
            return -1;
        for (int p = index + direction; p >= 0 && p < (int) entries.size(); p += direction)
            if (predicate (entries[(size_t) p]))
                return p;
        return -1;
    }

    void commit (int position)
    {
        if (isValidPosition (position))
            index = position;
    }

    void clear()
    {
        entries.clear();
        index = -1;
    }

    int size() const { return (int) entries.size(); }

private:
    std::vector<Entry> entries;
    int index = -1;
};
