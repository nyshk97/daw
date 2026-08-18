#pragma once

#include <optional>
#include <juce_core/juce_core.h>

// yt-dlp の出力を読むための純関数群。プロセス起動にもGUIにも依存しないので daw_tests から直接テストする。
// 書式は yt-dlp 2026.07.04 の実出力に合わせている（docs/plans/2026-07-26-url-audio-import.md のログ参照）
namespace YtDlpOutput
{
inline const char* progressPrefix = "LALA_PROGRESS";
inline const char* metaPrefix     = "LALA_META:";

namespace detail
{
// バイト数トークン。yt-dlp は値が無いとき "NA" を出すので、数字だけの並びかを見て弾く
inline std::optional<double> readBytes (const juce::String& token)
{
    if (token.isEmpty() || ! token.containsOnly ("0123456789"))
        return {};
    return token.getDoubleValue();
}

inline bool isUrlChar (juce::juce_wchar c)
{
    return ! juce::CharacterFunctions::isWhitespace (c)
           && c != '"' && c != '\'' && c != '<' && c != '>' && c != '`';
}

// URLの直後に付きがちな句読点。URL自体に含まれることもあるが、伏せる用途では切りすぎる方が安全
inline bool isTrailingPunct (juce::juce_wchar c)
{
    return c == '.' || c == ',' || c == ':' || c == ';' || c == ')' || c == ']' || c == '}';
}

inline juce::String ellipsis()
{
    return juce::String::fromUTF8 (u8"…");
}

// scheme://host + パスの先頭1セグメント まで残し、以降（残りのパス・query・fragment）を落とす。
// authority に userinfo（user:password@）があればそこも伏せる — サイトを制限しない仕様上、
// 認証情報付きURLが渡されうるため、ホスト名だけ残してその手前は消す
inline juce::String shortenUrl (const juce::String& url)
{
    const auto schemeEnd = url.indexOf ("://");
    if (schemeEnd < 0)
        return url;

    const int len = url.length();
    const int authorityStart = schemeEnd + 3;

    int authorityEnd = authorityStart;
    while (authorityEnd < len && url[authorityEnd] != '/' && url[authorityEnd] != '?'
           && url[authorityEnd] != '#')
        ++authorityEnd;

    const auto authority = url.substring (authorityStart, authorityEnd);
    const auto at = authority.lastIndexOfChar ('@'); // 区切りは最後の '@'
    juce::String result = url.substring (0, authorityStart)
                          + (at >= 0 ? ellipsis() + authority.substring (at) : authority);

    if (authorityEnd >= len)
        return result; // scheme://host だけ（userinfo は上で処理済み）

    if (url[authorityEnd] != '/')
        return result + ellipsis(); // host の直後が ? や #

    int seg = authorityEnd + 1;
    while (seg < len && url[seg] != '/' && url[seg] != '?' && url[seg] != '#')
        ++seg;

    result += url.substring (authorityEnd, seg); // パスの先頭1セグメント
    if (seg < len)
        result += ellipsis();
    return result;
}
}

// --progress-template "download:LALA_PROGRESS %(progress.downloaded_bytes)s
//                      %(progress.total_bytes)s %(progress.total_bytes_estimate)s"
// の1行を 0.0〜1.0 に読む。分母は total_bytes を優先し、NA なら total_bytes_estimate を使う
// （実測では総サイズが判っている動画では total_bytes 側だけが入り、estimate は NA になる）
inline std::optional<float> parseProgress (const juce::String& line)
{
    const auto trimmed = line.trim();
    if (! trimmed.startsWith (progressPrefix))
        return {};

    juce::StringArray tokens;
    tokens.addTokens (trimmed.fromFirstOccurrenceOf (progressPrefix, false, false).trim(), " ", "");
    tokens.removeEmptyStrings();
    if (tokens.size() < 2)
        return {};

    const auto done = detail::readBytes (tokens[0]);
    if (! done.has_value())
        return {};

    auto total = detail::readBytes (tokens[1]);
    if (! total.has_value() && tokens.size() >= 3)
        total = detail::readBytes (tokens[2]);
    if (! total.has_value() || *total <= 0.0)
        return {};

    return (float) juce::jlimit (0.0, 1.0, *done / *total);
}

struct Metadata
{
    bool         ok = false;           // prefix行が見つかり、JSONとして読めた
    juce::String title;
    double       durationSeconds = 0.0;
    bool         hasDuration = false;  // duration が有限の正数だった
    bool         isLive = false;
};

// --print "LALA_META:%(.{title,duration,is_live})j" の1行JSONを読む。
// 平文のフィールドを並べる方式と違い、タイトルに改行や prefix 文字列が入っても行が壊れない
inline Metadata parseMetadata (const juce::String& text)
{
    Metadata meta;
    const auto lines = juce::StringArray::fromLines (text);

    for (const auto& line : lines)
    {
        const auto trimmed = line.trim();
        if (! trimmed.startsWith (metaPrefix))
            continue;

        const auto json = juce::JSON::parse (trimmed.fromFirstOccurrenceOf (metaPrefix, false, false));
        if (! json.isObject())
            return meta; // prefix はあるが読めない → ok = false

        meta.ok = true;
        meta.title = json.getProperty ("title", juce::var()).toString().trim();

        const auto duration = json.getProperty ("duration", juce::var());
        if (duration.isDouble() || duration.isInt() || duration.isInt64())
        {
            const double seconds = (double) duration;
            // NaN / inf はこの比較の両方が false になるので一緒に弾ける
            if (seconds > 0.0 && seconds < 1.0e9)
            {
                meta.durationSeconds = seconds;
                meta.hasDuration = true;
            }
        }

        const auto live = json.getProperty ("is_live", juce::var());
        meta.isLive = live.isBool() && (bool) live;
        return meta;
    }
    return meta;
}

// ユーザーに見せる1行を選ぶ。yt-dlp は失敗理由を stderr の "ERROR: …" に出す
inline juce::String extractErrorLine (const juce::String& text)
{
    const auto lines = juce::StringArray::fromLines (text);

    for (const auto& line : lines)
    {
        const auto trimmed = line.trim();
        if (trimmed.contains ("ERROR:"))
            return trimmed;
    }
    for (int i = lines.size(); --i >= 0;)
    {
        const auto trimmed = lines[i].trim();
        if (trimmed.isNotEmpty())
            return trimmed;
    }
    return {};
}

// ダウンロード失敗が HTTP 403 かどうか。YouTube側の仕様変更で yt-dlp stable の
// デフォルト player client が丸ごと 403 になる時期があり（2026-08 実測）、その場合は
// 別クライアントでのリトライで通ることがある。リトライ判断に使う
inline bool isHttp403 (const juce::String& errorLine)
{
    return errorLine.contains ("HTTP Error 403");
}

// ログ・アラートに出す前にURLを伏せる。2段構えなのは実測した漏れ方が2種類あるため:
//   ① stdout の "[youtube] Extracting URL: https://…?v=xxx" → scheme付きなのでパターンで拾える
//   ② stderr の "[generic] foo?token=SECRET#frag: Unable to download webpage…"
//      → scheme も host も落ちてクエリだけ残るのでパターンでは拾えない。
//        渡した元URLは既知なので、その "?" 以降を直接伏せる。
//        query が無く fragment だけのURL（…/foo#SECRET）もあるので "#" 以降も別に伏せる
inline juce::String redactUrls (const juce::String& text, const juce::String& originalUrl = {})
{
    juce::String out;
    const int len = text.length();
    int i = 0;

    while (i < len)
    {
        const int http  = text.indexOf (i, "http://");
        const int https = text.indexOf (i, "https://");

        int start = -1;
        if (http >= 0 && https >= 0)  start = juce::jmin (http, https);
        else if (http >= 0)           start = http;
        else                          start = https;

        if (start < 0)
        {
            out += text.substring (i);
            break;
        }

        out += text.substring (i, start);

        int end = start;
        while (end < len && detail::isUrlChar (text[end]))
            ++end;
        while (end > start && detail::isTrailingPunct (text[end - 1]))
            --end;

        out += detail::shortenUrl (text.substring (start, end));
        i = end;
    }

    // query（あれば fragment 込みの長い方）を先に、残った fragment を後で伏せる
    const auto queryAndFragment = originalUrl.fromFirstOccurrenceOf ("?", true, false);
    if (queryAndFragment.isNotEmpty() && out.contains (queryAndFragment))
        out = out.replace (queryAndFragment, "?" + detail::ellipsis());

    const auto fragment = originalUrl.fromFirstOccurrenceOf ("#", true, false);
    if (fragment.isNotEmpty() && out.contains (fragment))
        out = out.replace (fragment, "#" + detail::ellipsis());

    return out;
}
}
