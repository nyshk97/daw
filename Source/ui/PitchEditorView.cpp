#include "PitchEditorView.h"

#include <cmath>

#include "Fonts.h"
#include "Shortcuts.h"
#include "Theme.h"
#include "../shared/Log.h"

namespace
{
juce::String jp (const char* text) { return juce::String::fromUTF8 (text); }

const juce::Colour blobFill { 0xff4a6ea9 };
const juce::Colour blobSelected { 0xff5b82c4 };
const juce::Colour blobBypass { 0xff55555a };
const juce::Colour blobBorder { 0xff7a9ede };
const juce::Colour bannerBgColour { 0xff3a3422 };
const juce::Colour bannerTextColour { 0xffe9d6a0 };
const juce::Colour bannerButtonColour { 0xffdfae4a };
const juce::Colour hatchGrow { 0xffdfae4a };
const juce::Colour hatchShrink { 0xff5a8ce6 };
constexpr int scaleItemProjectKey = 1, scaleItemChromatic = 2, scaleItemCustomBase = 10;
} // namespace

PitchEditorView::PitchEditorView()
{
    setWantsKeyboardFocus (true);
    auto setupLabel = [this] (juce::Label& l, const char* text)
    {
        l.setText (jp (text), juce::dontSendNotification);
        l.setFont (Fonts::small());
        l.setColour (juce::Label::textColourId, Theme::topBarIcon);
        l.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (l);
    };
    setupLabel (scaleLabel, u8"Scale");
    setupLabel (strengthLabel, u8"Strength");
    setupLabel (speedLabel, u8"Speed");
    statusLabel.setFont (Fonts::small());
    statusLabel.setColour (juce::Label::textColourId, Theme::topBarIcon);
    statusLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (statusLabel);

    auto setupButton = [this] (juce::TextButton& b, const char* text, bool primary = false)
    {
        b.setButtonText (jp (text));
        b.setColour (juce::TextButton::buttonColourId, primary ? Theme::topBarIcon : juce::Colours::transparentBlack);
        b.setColour (juce::TextButton::textColourOffId, primary ? juce::Colours::white : Theme::topBarIcon);
        b.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        b.setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (b);
    };
    setupButton (resnapButton, u8"Re-snap");
    setupButton (keroButton, u8"Hard tune");
    keroButton.setTooltip (jp (u8"ケロケロ（Auto-Tune のハードチューン）: Speed 0ms・Strength 100% にする"));
    setupButton (enableButton, u8"Enable", true);
    setupButton (applyButton, u8"Apply", true);
    setupButton (cancelButton, u8"Cancel");
    cancelButton.setColour (juce::TextButton::buttonColourId, Theme::recordRed);
    cancelButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    setupButton (retryButton, u8"Retry");
    scaleHighlightButton.setTooltip (jp (u8"スケール音の段をハイライト（キー未設定時は推定キー）"));
    scaleHighlightButton.setClickingTogglesState (true);
    scaleHighlightButton.setToggleState (highlightScale, juce::dontSendNotification);
    scaleHighlightButton.setOnLightBackground (true);
    scaleHighlightButton.setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    scaleHighlightButton.onClick = [this] { highlightScale = scaleHighlightButton.getToggleState(); repaint(); };
    addAndMakeVisible (scaleHighlightButton);

    scaleBox.addItem (jp (u8"Project key"), scaleItemProjectKey);
    scaleBox.addItem (jp (u8"Chromatic"), scaleItemChromatic);
    for (int root = 0; root < 12; ++root)
        for (int minor = 0; minor < 2; ++minor)
            scaleBox.addItem (ProjectKeys::displayName ({ root, minor ? KeyMode::minor : KeyMode::major }),
                              scaleItemCustomBase + root * 2 + minor);
    scaleBox.onChange = [this] { applyScaleSelection(); };
    addAndMakeVisible (scaleBox);

    auto setupSlider = [this] (juce::Slider& s, double lo, double hi, double step)
    {
        s.setSliderStyle (juce::Slider::LinearHorizontal);
        s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
        s.setRange (lo, hi, step);
        s.setScrollWheelEnabled (false);
        s.setColour (juce::Slider::textBoxTextColourId, Theme::topBarIcon);
        s.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        s.onDragStart = [this] { if (canEdit() && onBeginEdit) { sliderEditing = true; onBeginEdit(); } };
        s.onDragEnd = [this] { sliderEditing = false; };
        addAndMakeVisible (s);
    };
    setupSlider (strengthSlider, 0.0, 100.0, 1.0);
    strengthSlider.setTextValueSuffix ("%");
    setupSlider (speedSlider, 0.0, 400.0, 1.0);
    speedSlider.setTextValueSuffix (" ms");
    strengthSlider.onValueChange = [this]
    {
        if (! canEdit() || session == nullptr) return;
        const float v = (float) (strengthSlider.getValue() / 100.0);
        if (juce::exactlyEqual (v, session->working().strength)) return;
        if (! sliderEditing && onBeginEdit) onBeginEdit(); // テキスト入力等のクリック列外の経路も区切る
        session->mutableWorking().strength = v;
        if (onEdited) onEdited();
    };
    speedSlider.onValueChange = [this]
    {
        if (! canEdit() || session == nullptr) return;
        const float v = (float) speedSlider.getValue();
        if (juce::exactlyEqual (v, session->working().speedMs)) return;
        if (! sliderEditing && onBeginEdit) onBeginEdit();
        session->mutableWorking().speedMs = v;
        if (onEdited) onEdited();
    };
    keroButton.onClick = [this]
    {
        if (! canEdit() || session == nullptr) return;
        auto& w = session->mutableWorking();
        if (juce::exactlyEqual (w.speedMs, PitchCorrection::keroSpeedMs) && juce::exactlyEqual (w.strength, 1.0f)) return;
        if (onBeginEdit) onBeginEdit();
        w.speedMs = PitchCorrection::keroSpeedMs;
        w.strength = 1.0f;
        updateBar();
        if (onEdited) onEdited();
    };
    resnapButton.onClick = [this] { if (onResnap) onResnap(); };
    enableButton.onClick = [this] { if (onEnable) onEnable(); };
    applyButton.onClick = [this] { if (onApply) onApply(); };
    cancelButton.onClick = [this] { if (onCancel) onCancel(); };
    retryButton.onClick = [this] { if (onRetrySidecar) onRetrySidecar(); };

    bannerLabel.setFont (Fonts::small());
    bannerLabel.setColour (juce::Label::textColourId, bannerTextColour);
    addChildComponent (bannerLabel);
    bannerButton.setColour (juce::TextButton::buttonColourId, bannerButtonColour);
    bannerButton.setColour (juce::TextButton::textColourOffId, Theme::topBarIcon);
    bannerButton.onClick = [this]
    {
        if (session == nullptr) return;
        if (session->mode() == PitchEditorSession::Mode::initialPreview && onEnable) onEnable();
        else if (session->sidecarBlocked() && onRetrySidecar) onRetrySidecar();
    };
    addChildComponent (bannerButton);
    bannerKeyButton.setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    bannerKeyButton.setColour (juce::TextButton::textColourOffId, bannerTextColour);
    bannerKeyButton.setColour (juce::ComboBox::outlineColourId, juce::Colour (0xff8a7a44));
    bannerKeyButton.onClick = [this] { if (onSetProjectKey) onSetProjectKey(); };
    addChildComponent (bannerKeyButton);

    startTimerHz (30); // 再生ヘッド追従・減光の更新（pull 型）
}

PitchEditorView::~PitchEditorView() { stopTimer(); }


void PitchEditorView::showStatus (const juce::String& text)
{
    statusText = text;
    statusUntil = juce::Time::getMillisecondCounterHiRes() + 4000.0;
    statusLabel.setText (text, juce::dontSendNotification);
}

void PitchEditorView::refresh()
{
    updateBar();
    resized();
    repaint();
}

void PitchEditorView::updateBar()
{
    using Mode = PitchEditorSession::Mode;
    const auto mode = session != nullptr ? session->mode() : Mode::closed;
    const bool editable = canEdit();
    const bool committed = mode == Mode::committed;
    const bool changing = mode == Mode::changePreview;

    scaleBox.setEnabled (editable);
    resnapButton.setEnabled (committed && editable);
    keroButton.setEnabled (editable);
    strengthSlider.setEnabled (editable);
    speedSlider.setEnabled (editable);
    // 右端＝確定系。未確定プレビュー中だけ Enable、変更プレビュー中だけ Apply/Cancel。確定後は何も出さない
    //（状態は ♪ バッジとバナーの有無で分かる。Reset / Re-analyze / Set project key は右クリックメニュー）
    enableButton.setVisible (mode == Mode::initialPreview);
    enableButton.setEnabled (mode == Mode::initialPreview && editable);
    applyButton.setVisible (changing);
    cancelButton.setVisible (changing);
    retryButton.setVisible (false);

    if (session != nullptr && mode != Mode::closed && mode != Mode::analyzing)
    {
        const auto& w = session->working();
        strengthSlider.setValue (w.strength * 100.0, juce::dontSendNotification);
        speedSlider.setValue (w.speedMs, juce::dontSendNotification);
        int item = scaleItemProjectKey;
        if (w.scaleMode == PitchScaleMode::chromatic) item = scaleItemChromatic;
        else if (w.scaleMode == PitchScaleMode::custom)
            item = scaleItemCustomBase + w.customKey.root * 2 + (w.customKey.mode == KeyMode::minor ? 1 : 0);
        scaleBox.setSelectedId (item, juce::dontSendNotification);
        // プロジェクトキー未設定なら推定値を項目名に出す（設定はバナーのボタンか右クリック）
        const auto projectKey = getProjectKey ? getProjectKey() : std::nullopt;
        if (projectKey.has_value())
            scaleBox.changeItemText (scaleItemProjectKey, jp (u8"Project key: ") + ProjectKeys::displayName (*projectKey));
        else
        {
            const auto est = PitchNotes::estimateKey (session->detected());
            scaleBox.changeItemText (scaleItemProjectKey,
                                     est.valid ? jp (u8"Project key: unset (guess ") + ProjectKeys::displayName (est.key) + ")"
                                               : jp (u8"Project key: unset"));
        }
    }

    // バナー
    bannerVisible = true;
    bannerButton.setVisible (false);
    if (session != nullptr && session->sidecarBlocked())
    {
        const auto why = getBlockedMessage ? getBlockedMessage() : juce::String();
        bannerText = jp (u8"解析結果を保存できなかったため編集できません") + (why.isNotEmpty() ? " — " + why : juce::String());
        bannerButton.setButtonText (jp (u8"Retry"));
        bannerButton.setVisible (true);
    }
    else if (mode == Mode::analyzing)
        bannerText = jp (u8"解析中…");
    else if (mode == Mode::initialPreview)
    {
        bannerText = jp (u8"未確定のプレビュー（自動スナップ）— Space で試聴・最初の編集か Enable で確定");
        bannerButton.setButtonText (jp (u8"Enable"));
        bannerButton.setVisible (true);
    }
    bannerKeyButton.setVisible (false);
    if (session != nullptr && (mode == Mode::initialPreview || committed) && getProjectKey && ! getProjectKey().has_value())
    {
        // キー未設定: 推定キーを文で見せ、1クリックでプロジェクトに設定できるようにする
        if (const auto est = PitchNotes::estimateKey (session->detected()); est.valid)
        {
            const auto keyText = ProjectKeys::displayName (est.key);
            if (! bannerVisible)
                bannerText = juce::String();
            bannerVisible = true;
            bannerText = (bannerText.isNotEmpty() ? bannerText + jp (u8"　｜　") : juce::String())
                       + jp (u8"プロジェクトキー未設定。推定: ") + keyText
                       + (est.correlation < 0.5 ? jp (u8"（確度は低め）") : juce::String());
            bannerKeyButton.setButtonText (jp (u8"Use ") + keyText);
            bannerKeyButton.setVisible (editable);
        }
    }
    else if (changing)
        bannerText = jp (u8"変更のプレビュー中 — Apply で確定・Cancel で元に戻す（この間は編集できません）");
    else
        bannerVisible = false;
    bannerLabel.setText (bannerText, juce::dontSendNotification);
    bannerLabel.setVisible (bannerVisible);
    if (juce::Time::getMillisecondCounterHiRes() > statusUntil)
        statusLabel.setText (mode == Mode::closed ? jp (u8"No clip") : juce::String(), juce::dontSendNotification);
}

void PitchEditorView::applyScaleSelection()
{
    if (! canEdit() || session == nullptr)
        return;
    auto& w = session->mutableWorking();
    const int id = scaleBox.getSelectedId();
    PitchScaleMode mode = PitchScaleMode::projectKey;
    ProjectKey key = w.customKey;
    if (id == scaleItemChromatic) mode = PitchScaleMode::chromatic;
    else if (id >= scaleItemCustomBase)
    {
        mode = PitchScaleMode::custom;
        key = { (id - scaleItemCustomBase) / 2, ((id - scaleItemCustomBase) % 2) ? KeyMode::minor : KeyMode::major };
    }
    if (w.scaleMode == mode && (mode != PitchScaleMode::custom || w.customKey == key))
        return;
    if (onBeginEdit) onBeginEdit();
    w.scaleMode = mode;
    w.customKey = key;
    if (onEdited) onEdited();
}

void PitchEditorView::resized()
{
    auto bar = getLocalBounds().removeFromTop (barHeight).reduced (8, 6);
    // 左: 音の決め方
    scaleLabel.setBounds (bar.removeFromLeft (36));
    scaleBox.setBounds (bar.removeFromLeft (190).reduced (2, 0));
    scaleHighlightButton.setBounds (bar.removeFromLeft (28).reduced (2, 1));
    resnapButton.setBounds (bar.removeFromLeft (64));
    bar.removeFromLeft (8);
    strengthLabel.setBounds (bar.removeFromLeft (52));
    strengthSlider.setBounds (bar.removeFromLeft (150));
    speedLabel.setBounds (bar.removeFromLeft (44));
    speedSlider.setBounds (bar.removeFromLeft (160));
    keroButton.setBounds (bar.removeFromLeft (76));
    // 右: 確定系（右端＝確定）
    auto right = bar;
    const auto primary = right.removeFromRight (changingButtonsVisible() ? 64 : 90);
    applyButton.setBounds (primary);
    enableButton.setBounds (primary);
    if (cancelButton.isVisible())
        cancelButton.setBounds (right.removeFromRight (64).reduced (2, 0));
    right.removeFromRight (8);
    statusLabel.setBounds (right);

    if (bannerVisible)
    {
        auto banner = getLocalBounds().withTop (barHeight).removeFromTop (bannerHeight).reduced (10, 3);
        if (bannerButton.isVisible())
            bannerButton.setBounds (banner.removeFromRight (64));
        if (bannerKeyButton.isVisible())
            bannerKeyButton.setBounds (banner.removeFromRight (84).reduced (4, 0));
        bannerLabel.setBounds (banner);
    }
}

bool PitchEditorView::changingButtonsVisible() const
{
    return session != nullptr && session->mode() == PitchEditorSession::Mode::changePreview;
}

PitchEditorView::Geometry PitchEditorView::computeGeometry (const Clip& clip) const
{
    Geometry g;
    auto area = getLocalBounds().withTop (barHeight + (bannerVisible ? bannerHeight : 0));
    area.removeFromTop (rulerHeight);
    area.removeFromLeft (keyboardWidth);
    g.canvas = area;
    g.domainOffset = clip.requestedDomainOffset();
    g.domainLength = clip.requestedDomainLength();
    g.stretchRatio = clip.stretchRatio;
    const double sr = getSampleRate ? getSampleRate() : 48000.0;
    if (session != nullptr && session->isOpen() && session->mode() != PitchEditorSession::Mode::analyzing)
        g.timeMap = PitchCorrections::buildTimeMap (session->working(), g.domainOffset, g.domainLength, g.stretchRatio, sr);
    else
        g.timeMap = TimeMap::uniform (g.domainOffset, g.domainLength, g.stretchRatio);
    // クリップの view 範囲（render 座標）をズーム倍率で切り出し、scrollRender ぶんずらす
    const auto clipStart = g.timeMap.map (clip.offsetSamples);
    const auto clipEnd = juce::jmax (clipStart + 1, g.timeMap.map (clip.offsetSamples + clip.lengthSamples));
    const auto visible = juce::jmax ((juce::int64) 1, (juce::int64) std::llround ((double) (clipEnd - clipStart) / zoom));
    const auto maxScroll = juce::jmax ((juce::int64) 0, clipEnd - clipStart - visible);
    g.clipRenderStart = clipStart;
    g.viewStart = clipStart + juce::jlimit ((juce::int64) 0, maxScroll, scrollRender);
    g.viewEnd = g.viewStart + visible;
    g.pxPerSample = (double) juce::jmax (1, g.canvas.getWidth()) / (double) visible;

    // 音域: ノートの目標音（全部）と有声フレームの音程の 2〜98 パーセンタイル（オクターブ飛びの数フレームに
    // 引っ張られて段が潰れないように）から ±2 半音、最低 12 段
    double lo = 1e9, hi = -1e9;
    if (session != nullptr && session->isOpen())
    {
        for (const auto& n : session->working().notes) { lo = juce::jmin (lo, (double) n.targetMidi); hi = juce::jmax (hi, (double) n.targetMidi); }
        if (const auto& c = session->curve(); c != nullptr && c->hopSamples > 0)
        {
            std::vector<double> voiced;
            const int k0 = (int) (clip.offsetSamples / c->hopSamples), k1 = (int) ((clip.offsetSamples + clip.lengthSamples) / c->hopSamples);
            for (int k = juce::jmax (0, k0); k < juce::jmin (c->numFrames(), k1); ++k)
                if (c->isVoiced (k)) voiced.push_back (c->midiAt (k));
            if (! voiced.empty())
            {
                std::sort (voiced.begin(), voiced.end());
                lo = juce::jmin (lo, voiced[(size_t) ((double) (voiced.size() - 1) * 0.02)]);
                hi = juce::jmax (hi, voiced[(size_t) ((double) (voiced.size() - 1) * 0.98)]);
            }
        }
    }
    if (lo > hi) { lo = 55; hi = 67; }
    g.midiLo = juce::jlimit (12, 120, (int) std::floor (lo) - 2);
    g.midiHi = juce::jlimit (g.midiLo + 12, 127, (int) std::ceil (hi) + 3);
    g.rowHeight = (float) g.canvas.getHeight() / (float) (g.midiHi - g.midiLo);
    return g;
}

int PitchEditorView::noteAt (const Geometry& g, juce::Point<int> p) const
{
    if (session == nullptr || ! session->isOpen())
        return -1;
    const auto& w = session->working();
    for (int i = 0; i < (int) w.notes.size(); ++i)
    {
        const auto& n = w.notes[(size_t) i];
        const auto s = w.resolve (n.start, g.domainOffset, g.domainLength), e = w.resolve (n.end, g.domainOffset, g.domainLength);
        const float x0 = g.xForSource (s), x1 = g.xForSource (e);
        const float y0 = g.yForMidi (n.targetMidi + 0.5);
        if ((float) p.x >= x0 && (float) p.x <= x1 && (float) p.y >= y0 && (float) p.y <= y0 + g.rowHeight)
            return i;
    }
    return -1;
}

void PitchEditorView::drawKeyboard (juce::Graphics& g, const Geometry& geo) const
{
    static const char* names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    auto keys = juce::Rectangle<int> (0, geo.canvas.getY(), keyboardWidth, geo.canvas.getHeight());
    g.setColour (Theme::headerBg);
    g.fillRect (keys);
    for (int m = geo.midiLo; m < geo.midiHi; ++m)
    {
        const bool black = m % 12 == 1 || m % 12 == 3 || m % 12 == 6 || m % 12 == 8 || m % 12 == 10;
        const float y = geo.yForMidi (m + 1);
        g.setColour (black ? juce::Colour (0xff2b2b30) : juce::Colour (0xffd8d8dc));
        g.fillRect (0.0f, y, (float) keyboardWidth, geo.rowHeight - 1.0f);
        if (! black && geo.rowHeight >= 10.0f)
        {
            g.setColour (juce::Colour (0xff555555));
            g.setFont (Fonts::small().withHeight (juce::jmin (11.0f, geo.rowHeight - 1.0f)));
            g.drawText (juce::String (names[m % 12]) + juce::String (m / 12 - 1), 3, (int) y, keyboardWidth - 4, (int) geo.rowHeight - 1,
                        juce::Justification::centredLeft);
        }
    }
}

void PitchEditorView::drawGrid (juce::Graphics& g, const Geometry& geo, const Clip& clip) const
{
    // 段（スケール外を薄く）
    // スケール音の段を明るく・外を暗く（Hl トグル）。キー未設定・プロジェクトキー追従なら推定キーで示す
    std::optional<ProjectKey> scale;
    if (highlightScale && session != nullptr && session->isOpen())
    {
        scale = PitchCorrections::effectiveScale (session->working(), getProjectKey ? getProjectKey() : std::nullopt);
        if (! scale.has_value() && session->working().scaleMode == PitchScaleMode::projectKey)
            if (const auto est = PitchNotes::estimateKey (session->detected()); est.valid)
                scale = est.key;
    }
    for (int m = geo.midiLo; m < geo.midiHi; ++m)
    {
        const bool inScale = ! scale.has_value() || PitchCorrections::snapToScale (m, scale) == m;
        g.setColour (! scale.has_value() ? Theme::timelineBg : inScale ? juce::Colour (0xff2a2b33) : juce::Colour (0xff17171b));
        g.fillRect ((float) geo.canvas.getX(), geo.yForMidi (m + 1), (float) geo.canvas.getWidth(), geo.rowHeight);
        g.setColour (Theme::gridLineBeat);
        g.fillRect ((float) geo.canvas.getX(), geo.yForMidi (m), (float) geo.canvas.getWidth(), 1.0f);
    }
    // ルーラーと拍グリッド（タイムラインと同じ解像度上限 1/16）
    const double sr = getSampleRate ? getSampleRate() : 48000.0;
    const double bpm = getBpm ? getBpm() : 120.0;
    const double beat = sr * 60.0 / juce::jmax (20.0, bpm);
    const double sixteenth = beat / 4.0;
    const juce::int64 timelineStart = geo.timelineForRender (geo.viewStart, clip.startSample); // 表示左端のタイムライン位置（スクロール込み）
    auto ruler = juce::Rectangle<int> (geo.canvas.getX(), geo.canvas.getY() - rulerHeight, geo.canvas.getWidth(), rulerHeight);
    g.setColour (Theme::rulerBg);
    g.fillRect (ruler);
    const double pxPerSixteenth = sixteenth * geo.pxPerSample;
    const int step = pxPerSixteenth >= 6.0 ? 1 : pxPerSixteenth >= 1.5 ? 4 : 16; // 密なら拍・小節だけ
    const auto firstIndex = (juce::int64) std::floor ((double) timelineStart / sixteenth);
    for (juce::int64 i = firstIndex; ; i += step)
    {
        const double t = (double) i * sixteenth;
        const auto render = geo.viewStart + (juce::int64) std::llround (t - (double) timelineStart);
        if (render > geo.viewEnd) break;
        if (render < geo.viewStart) continue;
        const float x = geo.xForRender (render);
        const bool bar = i % 16 == 0, beatLine = i % 4 == 0;
        g.setColour (bar ? Theme::gridLineBar.brighter (0.6f) : beatLine ? Theme::gridLineBar : Theme::gridLineSub);
        g.fillRect (x, (float) geo.canvas.getY(), 1.0f, (float) geo.canvas.getHeight());
        if (bar)
        {
            g.setColour (Theme::rulerLabel);
            g.setFont (Fonts::mono (10.0f));
            g.drawText (juce::String (i / 16 + 1), (int) x + 3, ruler.getY(), 40, rulerHeight, juce::Justification::centredLeft);
        }
    }
}

void PitchEditorView::drawAbsorbHatch (juce::Graphics& g, const Geometry& geo) const
{
    if (! lastDragValid || lastDragForHatch.appliedDelta == 0)
        return;
    const auto& d = lastDragForHatch;
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float alpha = drag.has_value() ? 1.0f : (float) juce::jlimit (0.0, 1.0, (hatchFadeUntil - now) / 600.0);
    if (alpha <= 0.0f)
        return;
    auto hatch = [&] (juce::int64 a, juce::int64 b, bool grow, juce::int64 delta)
    {
        if (b <= a) return;
        const float x0 = geo.xForRender (a), x1 = geo.xForRender (b);
        g.saveState();
        g.reduceClipRegion (juce::Rectangle<float> (x0, (float) geo.canvas.getY(), x1 - x0, (float) geo.canvas.getHeight()).toNearestInt());
        g.setColour ((grow ? hatchGrow : hatchShrink).withAlpha (0.5f * alpha));
        const float h = (float) geo.canvas.getHeight();
        for (float x = x0 - h; x < x1 + h; x += 8.0f)
            g.drawLine (x, (float) geo.canvas.getBottom(), x + h, (float) geo.canvas.getY(), 1.0f);
        g.restoreState();
        g.setColour ((grow ? hatchGrow : hatchShrink).withAlpha (alpha));
        g.setFont (Fonts::small());
        const double sr = getSampleRate ? getSampleRate() : 48000.0;
        g.drawText ((grow ? "+" : juce::String::fromUTF8 (u8"−")) + juce::String ((int) std::llround (std::abs ((double) delta) / sr * 1000.0)) + " ms",
                    (int) ((x0 + x1) / 2) - 24, geo.canvas.getY() + 2, 48, 14, juce::Justification::centred);
    };
    // 手前の区間 [prevEnd, start) と後ろの区間 [end, nextStart)（ドラッグ開始時の位置を基準に Δ だけ変化）
    hatch (d.prevEndAtStart, d.startAtStart + d.appliedDelta, d.appliedDelta > 0, d.appliedDelta);
    hatch (d.endAtStart + d.appliedDelta, d.nextStartAtStart, d.appliedDelta < 0, d.appliedDelta);
    // ゴースト（元の位置）
    g.setColour (juce::Colours::white.withAlpha (0.18f * alpha));
    const float gx0 = geo.xForRender (d.startAtStart), gx1 = geo.xForRender (d.endAtStart);
    const float gy = geo.yForMidi (d.targetAtStart + 0.5);
    const float dash[] = { 2.0f, 3.0f };
    juce::Path p;
    p.addRectangle (gx0, gy, gx1 - gx0, geo.rowHeight);
    juce::PathStrokeType (1.0f).createDashedStroke (p, p, dash, 2);
    g.fillPath (p);
}

void PitchEditorView::drawBlobs (juce::Graphics& g, const Geometry& geo, const Clip& clip)
{
    if (session == nullptr || ! session->isOpen() || session->curve() == nullptr)
        return;
    const auto& w = session->working();
    const auto& curve = *session->curve();
    if (cachedTargetDigest != w.digest())
    {
        cachedTarget = PitchCorrections::targetCurve (w, curve, geo.domainOffset, geo.domainLength, clip.transposeSemitones);
        cachedTargetDigest = w.digest();
    }
    const bool dim = isRendering && isRendering();
    const float alpha = dim ? 0.55f : 1.0f;
    const int hop = curve.hopSamples;
    auto shiftAt = [&] (int k) -> float
    {
        const int i = k - cachedTarget.firstFrame;
        return i >= 0 && i < (int) cachedTarget.shiftSemitones.size() ? cachedTarget.shiftSemitones[(size_t) i] : (float) clip.transposeSemitones;
    };

    for (int i = 0; i < (int) w.notes.size(); ++i)
    {
        const auto& n = w.notes[(size_t) i];
        const auto s = w.resolve (n.start, geo.domainOffset, geo.domainLength), e = w.resolve (n.end, geo.domainOffset, geo.domainLength);
        if (e <= s) continue;
        const float x0 = geo.xForSource (s), x1 = geo.xForSource (e);
        if (x1 < (float) geo.canvas.getX() || x0 > (float) geo.canvas.getRight()) continue;
        const float y = geo.yForMidi (n.targetMidi + 0.5);
        const bool sel = selected.count (i) > 0;
        g.setColour ((n.bypass ? blobBypass : sel ? blobSelected : blobFill).withAlpha (alpha * (n.bypass ? 0.35f : 0.85f)));
        g.fillRect (x0, y, x1 - x0, geo.rowHeight);
        g.setColour ((sel ? juce::Colours::white : blobBorder).withAlpha (alpha));
        g.drawRect (juce::Rectangle<float> (x0, y, x1 - x0, geo.rowHeight), hoverNote == i ? 1.5f : 1.0f);
        // 元カーブ（薄）と補正後（濃）
        juce::Path orig, corrected;
        bool first = true;
        for (int k = (int) ((s + hop - 1) / hop); (juce::int64) k * hop < e && k < curve.numFrames(); ++k)
        {
            if (! curve.isVoiced (k)) { first = true; continue; }
            const float x = geo.xForSource ((juce::int64) k * hop);
            const double m = curve.midiAt (k);
            const float yo = geo.yForMidi (m), yc = geo.yForMidi (m + shiftAt (k));
            if (first) { orig.startNewSubPath (x, yo); corrected.startNewSubPath (x, yc); first = false; }
            else { orig.lineTo (x, yo); corrected.lineTo (x, yc); }
        }
        g.setColour (juce::Colours::white.withAlpha (0.35f * alpha));
        g.strokePath (orig, juce::PathStrokeType (1.0f));
        g.setColour (juce::Colours::white.withAlpha ((n.bypass ? 0.35f : 1.0f) * alpha));
        g.strokePath (corrected, juce::PathStrokeType (1.6f));
        if (n.bypass && geo.rowHeight >= 10.0f)
        {
            g.setFont (Fonts::small().withHeight (9.0f));
            g.setColour (juce::Colours::lightgrey.withAlpha (alpha));
            g.drawText ("bypass", (int) x0 + 3, (int) y - 11, 60, 10, juce::Justification::centredLeft);
        }
    }
    // ノート外の有声フレーム（検出で捨てられた短い音など）は補正されないことを点で示す
    g.setColour (juce::Colours::white.withAlpha (0.2f * alpha));
    const int k0 = (int) (clip.offsetSamples / hop), k1 = (int) ((clip.offsetSamples + clip.lengthSamples) / hop);
    for (int k = juce::jmax (0, k0); k < juce::jmin (curve.numFrames(), k1); ++k)
    {
        if (! curve.isVoiced (k)) continue;
        const auto sample = (juce::int64) k * hop;
        bool inNote = false;
        for (const auto& n : w.notes)
            if (sample >= w.resolve (n.start, geo.domainOffset, geo.domainLength) && sample < w.resolve (n.end, geo.domainOffset, geo.domainLength)) { inNote = true; break; }
        if (! inNote)
            g.fillRect (geo.xForSource (sample), geo.yForMidi (curve.midiAt (k)), 1.5f, 1.5f);
    }
    if (dim)
    {
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.setFont (Fonts::small());
        g.drawText (jp (u8"rendering…"), geo.canvas.getRight() - 90, geo.canvas.getY() + 4, 84, 14, juce::Justification::centredRight);
    }
}

void PitchEditorView::paint (juce::Graphics& g)
{
    g.fillAll (Theme::timelineBg);
    // 上部バー（シルバー）
    g.setGradientFill (juce::ColourGradient (Theme::topBarTop, 0, 0, Theme::topBarBottom, 0, (float) barHeight, false));
    g.fillRect (0, 0, getWidth(), barHeight);
    if (bannerVisible)
    {
        g.setColour (session != nullptr && session->sidecarBlocked() ? juce::Colour (0xff4a2a26) : bannerBgColour);
        g.fillRect (0, barHeight, getWidth(), bannerHeight);
    }
    const Clip* clip = getClip ? getClip() : nullptr;
    if (clip == nullptr || session == nullptr || ! session->isOpen())
    {
        g.setColour (Theme::rulerLabel);
        g.setFont (Fonts::body());
        g.drawText (jp (u8"リージョンを右クリック →「ピッチ補正…」で開きます"), getLocalBounds().withTop (barHeight), juce::Justification::centred);
        return;
    }
    const auto geo = computeGeometry (*clip);
    drawGrid (g, geo, *clip);
    drawKeyboard (g, geo);
    if (session->mode() == PitchEditorSession::Mode::analyzing)
    {
        const float p = getAnalysisProgress ? getAnalysisProgress() : 0.0f;
        g.setColour (Theme::rulerLabel);
        g.setFont (Fonts::body());
        g.drawText (jp (u8"解析中… ") + juce::String ((int) (p * 100)) + "%", geo.canvas, juce::Justification::centred);
        g.setColour (Theme::accent);
        g.fillRect ((float) geo.canvas.getX(), (float) geo.canvas.getBottom() - 3.0f, (float) geo.canvas.getWidth() * p, 3.0f);
    }
    else
    {
        drawAbsorbHatch (g, geo);
        drawBlobs (g, geo, *clip);
    }
    // 再生ヘッド（メインと連動）
    if (getPlayheadSample)
    {
        const auto render = geo.renderForTimeline (getPlayheadSample(), clip->startSample);
        if (render >= geo.viewStart && render <= geo.viewEnd)
        {
            g.setColour (Theme::playhead);
            g.fillRect (geo.xForRender (render), (float) geo.canvas.getY() - rulerHeight, 1.5f, (float) geo.canvas.getHeight() + rulerHeight);
        }
    }
}

// ---- 操作 ----

void PitchEditorView::clampScroll (const Clip& clip)
{
    const auto geo = computeGeometry (clip);
    scrollRender = geo.viewStart - geo.timeMap.map (clip.offsetSamples); // computeGeometry がクランプ済み
}

void PitchEditorView::zoomAround (double factor, float anchorX)
{
    const Clip* clip = getClip ? getClip() : nullptr;
    if (clip == nullptr) return;
    const auto before = computeGeometry (*clip);
    const auto anchorRender = before.renderForX (anchorX);
    zoom = juce::jlimit (1.0, 64.0, zoom * factor);
    // アンカー（カーソル位置）の render 座標が同じ x に留まるようスクロールを合わせる
    const auto clipStart = before.timeMap.map (clip->offsetSamples);
    const auto clipEnd = juce::jmax (clipStart + 1, before.timeMap.map (clip->offsetSamples + clip->lengthSamples));
    const double visible = (double) (clipEnd - clipStart) / zoom;
    const double frac = (anchorX - (float) before.canvas.getX()) / (double) juce::jmax (1, before.canvas.getWidth());
    scrollRender = (juce::int64) std::llround ((double) (anchorRender - clipStart) - frac * visible);
    clampScroll (*clip);
    repaint();
}

void PitchEditorView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const Clip* clip = getClip ? getClip() : nullptr;
    if (clip == nullptr) return;
    if (e.mods.isCommandDown())
    {
        // ⌘＋ホイール＝ズーム（上で拡大）
        zoomAround (std::pow (1.5, (double) wheel.deltaY * 4.0), (float) e.x);
        return;
    }
    const auto geo = computeGeometry (*clip);
    const double dx = (std::abs (wheel.deltaX) > std::abs (wheel.deltaY) ? wheel.deltaX : wheel.deltaY);
    scrollRender -= (juce::int64) std::llround (dx * 300.0 / geo.pxPerSample);
    clampScroll (*clip);
    repaint();
}

void PitchEditorView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    zoomAround ((double) scaleFactor, (float) e.x);
}

void PitchEditorView::mouseMove (const juce::MouseEvent& e)
{
    const Clip* clip = getClip ? getClip() : nullptr;
    const int h = clip != nullptr ? noteAt (computeGeometry (*clip), e.getPosition()) : -1;
    if (h != hoverNote) { hoverNote = h; repaint(); }
}

void PitchEditorView::showContextMenu (juce::Point<int>)
{
    if (session == nullptr || ! session->isOpen())
        return;
    const bool committed = session->mode() == PitchEditorSession::Mode::committed && session->editable();
    juce::PopupMenu menu;
    menu.addItem (1, jp (u8"自動スナップからやり直す（手直しを捨てる）"), committed);
    menu.addItem (2, jp (u8"再解析（検出をやり直す。検出器が同じなら変わらない）"), committed);
    if (getProjectKey && ! getProjectKey().has_value())
        if (const auto est = PitchNotes::estimateKey (session->detected()); est.valid)
            menu.addItem (3, jp (u8"推定キー ") + ProjectKeys::displayName (est.key) + jp (u8" をプロジェクトに設定"), session->editable());
    juce::Component::SafePointer<PitchEditorView> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options(), [safe] (int result)
    {
        if (safe == nullptr) return;
        if (result == 1 && safe->onReset) safe->onReset();
        else if (result == 2 && safe->onReanalyze) safe->onReanalyze();
        else if (result == 3 && safe->onSetProjectKey) safe->onSetProjectKey();
    });
}

void PitchEditorView::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    const Clip* clip = getClip ? getClip() : nullptr;
    if (clip == nullptr || session == nullptr || ! session->isOpen())
        return;
    if (e.mods.isPopupMenu())
    {
        showContextMenu (e.getPosition());
        return;
    }
    const auto geo = computeGeometry (*clip);
    if (! geo.canvas.contains (e.getPosition()))
        return;
    const int idx = noteAt (geo, e.getPosition());
    if (idx < 0)
    {
        selected.clear();
        repaint();
        return;
    }
    auto& w = session->mutableWorking();
    if (e.mods.isCommandDown())
    {
        // ⌘クリック = 分割（現在の timeMap 上へ補間ノード挿入＝音は変わらない）
        if (! canEdit()) return;
        const auto render = geo.renderForX ((float) e.x);
        const auto source = geo.timeMap.inverse (render);
        if (onBeginEdit) onBeginEdit();
        const double sr = getSampleRate ? getSampleRate() : 48000.0;
        if (PitchCorrections::splitNote (w, idx, source, geo.domainOffset, geo.domainLength, geo.stretchRatio, sr))
        {
            Log::info ("pitch.split", "note=" + juce::String (idx));
            selected.clear();
            if (onEdited) onEdited();
        }
        repaint();
        return;
    }
    if (e.mods.isShiftDown())
        selected.insert (idx);
    else
    {
        selected.clear();
        selected.insert (idx);
    }
    Drag d;
    d.noteIndex = idx;
    d.start = e.getPosition();
    d.targetAtStart = w.notes[(size_t) idx].targetMidi;
    d.snap = ! e.mods.isAltDown();
    d.stateAtStart = w;
    const auto s = w.resolve (w.notes[(size_t) idx].start, geo.domainOffset, geo.domainLength);
    const auto en = w.resolve (w.notes[(size_t) idx].end, geo.domainOffset, geo.domainLength);
    d.startAtStart = geo.timeMap.map (s);
    d.endAtStart = geo.timeMap.map (en);
    d.prevEndAtStart = idx > 0 ? geo.timeMap.map (w.resolve (w.notes[(size_t) idx - 1].start, geo.domainOffset, geo.domainLength)) : geo.viewStart;
    if (idx > 0)
    {
        const auto pe = w.resolve (w.notes[(size_t) idx - 1].end, geo.domainOffset, geo.domainLength);
        d.prevEndAtStart = pe == s ? geo.timeMap.map (w.resolve (w.notes[(size_t) idx - 1].start, geo.domainOffset, geo.domainLength)) : geo.timeMap.map (pe);
    }
    d.nextStartAtStart = geo.timeMap.map (geo.domainOffset + geo.domainLength);
    if (idx + 1 < (int) w.notes.size())
    {
        const auto ns = w.resolve (w.notes[(size_t) idx + 1].start, geo.domainOffset, geo.domainLength);
        d.nextStartAtStart = ns == en ? geo.timeMap.map (w.resolve (w.notes[(size_t) idx + 1].end, geo.domainOffset, geo.domainLength)) : geo.timeMap.map (ns);
    }
    drag = d;
    repaint();
}

void PitchEditorView::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag.has_value() || session == nullptr || ! canEdit())
        return;
    const Clip* clip = getClip ? getClip() : nullptr;
    if (clip == nullptr) return;
    auto& d = *drag;
    const auto geo = computeGeometry (*clip);
    const int dx = e.x - d.start.x, dy = e.y - d.start.y;
    if (d.mode == Drag::Mode::undecided)
    {
        if (std::abs (dx) < 4 && std::abs (dy) < 4) return;
        d.mode = std::abs (dy) > std::abs (dx) ? Drag::Mode::pitch : Drag::Mode::time;
        if (onBeginEdit) onBeginEdit(); // 1ドラッグ = 1件（区切りはドラッグの開始）
    }
    auto& w = session->mutableWorking();
    if (d.noteIndex < 0 || d.noteIndex >= (int) w.notes.size()) return;
    if (d.mode == Drag::Mode::pitch)
    {
        const int target = juce::jlimit (0, 127, d.targetAtStart - (int) std::lround ((double) dy / geo.rowHeight));
        if (target != w.notes[(size_t) d.noteIndex].targetMidi)
        {
            w.notes[(size_t) d.noteIndex].targetMidi = target;
            d.moved = true;
            repaint();
        }
    }
    else
    {
        // 横: 目標位置（render）。⌥なしは 1/16 グリッドへ（タイムライン座標で）スナップ
        juce::int64 wantDelta = (juce::int64) std::llround ((double) dx / geo.pxPerSample);
        if (d.snap)
        {
            const double sr = getSampleRate ? getSampleRate() : 48000.0;
            const double bpm = getBpm ? getBpm() : 120.0;
            const double sixteenth = sr * 60.0 / juce::jmax (20.0, bpm) / 4.0;
            const double timelinePos = (double) geo.timelineForRender (d.startAtStart + wantDelta, clip->startSample);
            const double snapped = std::round (timelinePos / sixteenth) * sixteenth;
            wantDelta = geo.renderForTimeline ((juce::int64) std::llround (snapped), clip->startSample) - d.startAtStart;
        }
        const auto step = wantDelta - d.appliedDelta;
        if (step != 0)
        {
            const double sr = getSampleRate ? getSampleRate() : 48000.0;
            const auto applied = PitchCorrections::moveNote (w, d.noteIndex, step, geo.domainOffset, geo.domainLength, geo.stretchRatio, sr);
            if (applied != 0)
            {
                d.appliedDelta += applied;
                d.moved = true;
                lastDragForHatch = d;
                lastDragValid = true;
                repaint();
            }
        }
    }
}

void PitchEditorView::mouseUp (const juce::MouseEvent& e)
{
    if (! drag.has_value())
        return;
    auto d = *drag;
    drag.reset();
    if (d.moved)
    {
        Log::info ("pitch.drag", juce::String (d.mode == Drag::Mode::pitch ? "pitch" : "time") + " note=" + juce::String (d.noteIndex)
                                     + (d.mode == Drag::Mode::time ? " delta=" + juce::String (d.appliedDelta) : juce::String()));
        if (d.mode == Drag::Mode::time) { lastDragForHatch = d; lastDragValid = true; hatchFadeUntil = juce::Time::getMillisecondCounterHiRes() + 1400.0; }
        if (onEdited) onEdited();
    }
    else if (! e.mods.isShiftDown() && ! e.mods.isCommandDown() && d.mode == Drag::Mode::undecided)
    {
        // クリック = 単独試聴
        if (onAudition) onAudition (d.noteIndex);
    }
    repaint();
}

void PitchEditorView::mouseDoubleClick (const juce::MouseEvent& e)
{
    const Clip* clip = getClip ? getClip() : nullptr;
    if (clip == nullptr || session == nullptr || ! canEdit())
        return;
    const int idx = noteAt (computeGeometry (*clip), e.getPosition());
    if (idx < 0) return;
    if (onBeginEdit) onBeginEdit();
    auto& n = session->mutableWorking().notes[(size_t) idx];
    n.bypass = ! n.bypass;
    Log::info ("pitch.bypass", "note=" + juce::String (idx) + " on=" + juce::String ((int) n.bypass));
    if (onEdited) onEdited();
    repaint();
}

bool PitchEditorView::keyPressed (const juce::KeyPress& key)
{
    // ⌘←/→ = 横ズーム（タイムラインと同じ割り当て。ウィンドウ内ではこちらを優先し、メインへは流さない）
    if (Shortcuts::matches (key, Shortcuts::ID::zoomHorizontal))
    {
        const float centre = (float) getWidth() * 0.5f;
        zoomAround (key.getKeyCode() == juce::KeyPress::rightKey ? 1.5 : 1.0 / 1.5, centre);
        return true;
    }
    const bool plain = ! key.getModifiers().testFlags (juce::ModifierKeys::commandModifier | juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::altModifier);
    if (plain && key.getTextCharacter() == 'b' && canEdit() && ! selected.empty())
    {
        if (onBeginEdit) onBeginEdit();
        for (int idx : selected)
            if (idx >= 0 && idx < (int) session->working().notes.size())
                session->mutableWorking().notes[(size_t) idx].bypass = ! session->working().notes[(size_t) idx].bypass;
        if (onEdited) onEdited();
        repaint();
        return true;
    }
    if (plain && key.getTextCharacter() == 'm' && canEdit() && selected.size() == 2)
    {
        const Clip* clip = getClip ? getClip() : nullptr;
        if (clip == nullptr) return true;
        const int a = juce::jmin (*selected.begin(), *selected.rbegin()), b = juce::jmax (*selected.begin(), *selected.rbegin());
        if (b == a + 1)
        {
            if (onBeginEdit) onBeginEdit();
            const auto geo = computeGeometry (*clip);
            if (PitchCorrections::mergeNotes (session->mutableWorking(), a, geo.domainOffset, geo.domainLength))
            {
                Log::info ("pitch.merge", "left=" + juce::String (a));
                selected.clear();
                selected.insert (a);
                if (onEdited) onEdited();
            }
            repaint();
        }
        return true;
    }
    return onKey ? onKey (key) : false;
}
