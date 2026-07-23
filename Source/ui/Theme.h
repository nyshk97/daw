#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// アプリ全体の配色定義。UIの色はここを経由し、各コンポーネントに16進リテラルを直書きしない。
// 命名は「どこで使うか（役割）」基準。同じ値でも役割が違えば別名にする
// （例: markerLaneBg と gridLineSub は同値だが、片方だけ変えられるよう分けている）。
// アクセントの使い分け: UI状態（選択・ON・フォーカス）= accent（青）、
// トランスポート/ステータス（再生・録音・ソロ・メーター）= DAW慣習色（緑・赤・黄）。
namespace Theme
{
// ---- アクセント・状態色 ----
inline const juce::Colour accent        { 0xff4a6ea9 };  // 選択・ON・フォーカス（スライダー値・選択クリップ・クリックON・編集枠）
inline const juce::Colour muteOn        { 0xff5b82c4 };  // ミュートONボタン（accentより一段明るい青）
inline const juce::Colour soloOn        { 0xffdfae4a };  // ソロONボタン（DAW慣習の黄）
// 注意: juce::Colours::* から初期化しないこと。Colours::* はヘッダ内のTUローカルconst（非constexpr）で、
// inline変数の初期化順序次第でゼロ初期化（透明）を拾う（playheadが透明になる実害を確認済み）
inline const juce::Colour warning       { 0xffff4500 };  // SR不一致警告・エラーメッセージ（orangered）

// ---- トランスポート ----
inline const juce::Colour playGreen      { 0xff7bc47b };  // 再生中アイコン・レベルメーター
inline const juce::Colour recordRed      { 0xffd94a43 };  // 録音アイコン・メーターのクリップ表示
inline const juce::Colour recordActiveBg { 0xff8e2a26 };  // 録音中の録音ボタン背景（暗赤）
inline const juce::Colour recordGlow     { 0xffff5a4d };  // 録音中の明滅ハロー（recordActiveBgに埋もれない明るめの赤）

// ---- パネル背景・境界 ----
inline const juce::Colour windowBg          { 0xff2e2e33 };  // 上部バー・ウィンドウの地（LookAndFeel_V4デフォルトの青みグレーを無彩色に置き換え）
inline const juce::Colour timelineBg        { 0xff1e1e22 };  // タイムライン・ピアノロールの地
inline const juce::Colour rulerBg           { 0xff2a2a2e };  // 小節ルーラー
inline const juce::Colour markerLaneBg      { 0xff2a2a2e };  // セクションマーカーレーン（ルーラーと同色にして一枚のナビ帯に見せる）
inline const juce::Colour headerBg          { 0xff26262a };  // トラックヘッダー
inline const juce::Colour headerSelectedBg  { 0xff33333c };  // 選択トラックのヘッダー
inline const juce::Colour laneSelectedRowBg { 0xff26262e };  // 選択トラックのレーン行
inline const juce::Colour panelBorder       { 0xff2d2d32 };  // パネル・行の区切り線（主張しない程度に背景より一段明るいだけ）

// ---- ポップアップ（ツールチップ・オーバーレイ）----
inline const juce::Colour popupBg     { 0xff2c2c30 };
inline const juce::Colour popupBorder { 0xff55555a };

// ---- ルーラー・グリッド・再生ヘッド ----
inline const juce::Colour rulerTickBar  { 0xff55555a };  // 小節目盛り
inline const juce::Colour rulerTickBeat { 0xff4a4a4f };  // 拍目盛り
inline const juce::Colour rulerTickSub  { 0xff3c3c41 };  // 細分目盛り（1/8・1/16）
inline const juce::Colour rulerLabel    { 0xffd3d3d3 };  // 小節番号（lightgrey）
inline const juce::Colour playhead      { 0xffffffff };
// 小節>拍>細分の明度差をはっきり付ける（差が小さいと階層が目に効かず「均一な縦縞」に見える）
inline const juce::Colour gridLineBar   { 0xff3a3a40 };  // タイムラインの小節線
inline const juce::Colour gridLineBeat  { 0xff28282c };  // 拍線
inline const juce::Colour gridLineSub   { 0xff222226 };  // 細分線

// ---- サイクル（ループ範囲）帯 ----
// ルーラー上の範囲表示。ON=黄（Logicのサイクル準拠）、OFF=グレーで範囲を保持表示
inline const juce::Colour cycleOn  { 0xffd7b545 };
inline const juce::Colour cycleOff { 0xff77777e };

// ---- クリップ・リージョン ----
inline const juce::Colour clipAudio          { 0xff39537d };  // オーディオクリップ（選択時はaccent）
inline const juce::Colour clipMuted          { 0xff3c3d43 };
inline const juce::Colour clipMutedSelected  { 0xff54565e };
inline const juce::Colour regionMidi         { 0xff3a7350 };  // MIDIリージョン・ピアノロールの非選択ノート
inline const juce::Colour regionMidiSelected { 0xff4a9968 };

// ---- セクションマーカー ----
// 彩度は控えめにする（タグ然としたビビッドさを避け、地図情報として沈める）
inline const juce::Colour sectionIntro  { 0xffb5824d };  // オレンジ
inline const juce::Colour sectionVerse  { 0xff6b9e6d };  // 緑
inline const juce::Colour sectionHook   { 0xffbaa757 };  // 黄
inline const juce::Colour sectionBridge { 0xff5e88b8 };  // 青
inline const juce::Colour sectionOutro  { 0xffb86a60 };  // 赤茶
inline const juce::Colour sectionOther  { 0xff85858c };  // グレー

// ---- 音量バー・メーター（AppLookAndFeel::drawLinearSlider / StereoMeter。Logicのストリップ準拠）----
inline const juce::Colour faderSlotBg    { 0xff141417 };  // カプセル・フェーダー溝・メーター井戸の地
inline const juce::Colour knobTop        { 0xffa6a9af };  // 縦フェーダーキャップのグラデーション上端（マット気味）
inline const juce::Colour knobBottom     { 0xff8b8e94 };  // 同・下端
inline const juce::Colour knobBall       { 0xff9a9da3 };  // ヘッダーの球つまみ（フラット単色）
inline const juce::Colour faderRulerTick { 0xff4a4a50 };  // 縦フェーダー左の目盛り
inline const juce::Colour meterScaleText { 0xff7f7f88 };  // 縦メーターのdB数字
// メーターのレベル色。スケール位置に固定した緑→黄→赤（Logicと同じ読み方。
// グラデーションの組み立ては StereoMeter.h の Meters::gradient）
inline const juce::Colour meterGreenDeep { 0xff35935a };  // 根元（-60dB側）
inline const juce::Colour meterGreen     { 0xff4db06a };  // 〜-20dB帯
inline const juce::Colour meterYellow    { 0xffcbc94f };  // -13〜-8dB帯
inline const juce::Colour meterOrange    { 0xffd98f3e };  // -4dB付近
inline const juce::Colour meterRed       { 0xffd94a43 };  // 0dBFS直下（recordRedと同値だが役割が別）

// ---- FXパネル ----
inline const juce::Colour eqThumbCurve { 0xff7a9ede };  // EQサムネイルのカーブ線（暗い地の上で読めるようaccentより明るい青）
// PanノブはLogicのストリップ準拠: シルバーのノブ本体（knobTop/Bottom）の外周に暗色リングを敷き、
// センター起点の緑アークをリング上に重ねる。緑はplayGreenより彩度高め（Logicのパンゲージの見え方に合わせる）
inline const juce::Colour panRing     { 0xff3a3a40 };
inline const juce::Colour panArcGreen { 0xff55b85c };
// FXパネルのSends小ノブの値アーク（Logicのsendポットの緑。panArcGreenと同値だが役割が別）
inline const juce::Colour sendArcGreen { 0xff55b85c };

// ---- コントロール（ボタン・スライダー・LCD）----
inline const juce::Colour controlBg     { 0xff3f3f46 };  // M/Sボタン・スライダー溝
inline const juce::Colour controlTextOn { 0xff1c1c20 };  // ONボタン上の暗色文字
inline const juce::Colour lcdBg         { 0xff18181c };  // BPM/TIMEパネルの地
inline const juce::Colour lcdLabel      { 0xff7f7f88 };  // "BPM"/"TIME"の見出し
inline const juce::Colour lcdEditBg     { 0xff26262c };  // BPM編集中の背景

// ---- ピアノロール ----
inline const juce::Colour prWhiteKey       { 0xffd8d8dc };
inline const juce::Colour prBlackKey       { 0xff2a2a2e };
inline const juce::Colour prKeyOutline     { 0xff444448 };  // 鍵盤の区切り線
inline const juce::Colour prKeyLabelDark   { 0xff333338 };  // 白鍵上の音名・ドラム名
inline const juce::Colour prKeyLabelLight  { 0xffaaaaae };  // 黒鍵上のドラム名
inline const juce::Colour prKeyboardBorder { 0xff55555a };  // 鍵盤とノート領域の境界
inline const juce::Colour prRowDark        { 0xff1a1a1e };  // 黒鍵段の行背景
inline const juce::Colour prRegionTint     { 0xff2c3a30 };  // リージョン範囲の緑がかった地
inline const juce::Colour prLineStrong     { 0xff36363b };  // B/C境界・拍線
inline const juce::Colour prLineFaint      { 0xff26262a };  // 行区切り・細分線
inline const juce::Colour prGridBar        { 0xff4a4a50 };  // 小節線
inline const juce::Colour prVelocityBg     { 0xff232327 };  // ベロシティレーンの地
inline const juce::Colour prVelocityBorder { 0xff44444a };

// ---- プロジェクト選択画面 ----
inline const juce::Colour chooserPanelBg     { 0xff232327 };  // リストパネルの地（windowBgより一段沈める）
inline const juce::Colour chooserRowSelected { 0xff39537d };
inline const juce::Colour chooserRowHover    { 0xff2d2d33 };  // ホバー行（選択より弱く、気配程度）
inline const juce::Colour chooserMetaText    { 0xff85858c };  // 更新日時サブテキスト
inline const juce::Colour chooserTitleText   { 0xff9a9aa2 };  // 見出し（主役はリストなので沈める）
} // namespace Theme
