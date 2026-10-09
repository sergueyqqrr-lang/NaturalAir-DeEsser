#include "PluginEditor.h"

using namespace juce;

// ------------------------------------------------------------ look and feel
NaturalAirLookAndFeel::NaturalAirLookAndFeel()
{
    setColour(ResizableWindow::backgroundColourId, ui::bg);
    setColour(Label::textColourId, ui::text);
    setColour(Slider::textBoxTextColourId, ui::text);
    setColour(Slider::textBoxOutlineColourId, Colours::transparentBlack);
    setColour(ComboBox::backgroundColourId, ui::panel);
    setColour(ComboBox::textColourId, ui::text);
    setColour(ComboBox::outlineColourId, ui::dim.withAlpha(0.4f));
    setColour(ComboBox::arrowColourId, ui::dim);
    setColour(PopupMenu::backgroundColourId, ui::panel);
    setColour(PopupMenu::textColourId, ui::text);
}

void NaturalAirLookAndFeel::drawRotarySlider(Graphics& g, int x, int y, int w, int h, float pos,
                                             float a0, float a1, Slider& s)
{
    const bool big = dynamic_cast<AirKeepKnob*>(&s) != nullptr;
    auto b = Rectangle<float>((float) x, (float) y, (float) w, (float) h).reduced(big ? 26.f : 6.f);
    const float r = jmin(b.getWidth(), b.getHeight()) * 0.5f;
    const auto c = b.getCentre();
    const float th = big ? 9.f : 4.f;
    const float ar = r - th * 0.5f;

    g.setColour(ui::panel.brighter(0.08f));
    g.fillEllipse(c.x - r + th, c.y - r + th, (r - th) * 2, (r - th) * 2);

    Path track; track.addCentredArc(c.x, c.y, ar, ar, 0, a0, a1, true);
    g.setColour(ui::dim.withAlpha(0.25f));
    g.strokePath(track, PathStrokeType(th, PathStrokeType::curved, PathStrokeType::rounded));

    const float ang = a0 + pos * (a1 - a0);
    Path val; val.addCentredArc(c.x, c.y, ar, ar, 0, a0, ang, true);
    if (big) g.setGradientFill(ColourGradient(ui::amber, c.x - r, c.y + r, ui::cyan, c.x + r, c.y - r, false));
    else     g.setColour(ui::cyan.withAlpha(0.9f));
    g.strokePath(val, PathStrokeType(th, PathStrokeType::curved, PathStrokeType::rounded));

    const float pr = r - th * 2.2f;
    g.setColour(ui::text);
    g.drawLine(c.x + std::sin(ang) * pr * 0.45f, c.y - std::cos(ang) * pr * 0.45f,
               c.x + std::sin(ang) * pr, c.y - std::cos(ang) * pr, big ? 3.f : 2.f);
}

void NaturalAirLookAndFeel::drawToggleButton(Graphics& g, ToggleButton& b, bool hl, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced(1.f);
    const bool on = b.getToggleState();
    const bool isBypass = b.getButtonText() == "Bypass";
    const auto accent = isBypass ? ui::amber : ui::cyan;
    g.setColour(on ? accent.withAlpha(0.22f) : ui::panel);
    g.fillRoundedRectangle(r, 6.f);
    g.setColour(on ? accent : ui::dim.withAlpha(hl ? 0.8f : 0.45f));
    g.drawRoundedRectangle(r, 6.f, 1.2f);
    g.setColour(on ? accent : ui::dim);
    g.setFont(Font(FontOptions(13.f)));
    g.drawText(b.getButtonText(), r, Justification::centred);
}

void AirKeepKnob::paintOverChildren(Graphics& g)
{
    auto b = getLocalBounds().toFloat();
    b.removeFromBottom(22.f);
    const float r = jmin(b.getWidth(), b.getHeight()) * 0.5f - 6.f;
    const auto c = b.getCentre();
    const float a0 = degreesToRadians(0.f), span = MathConstants<float>::twoPi;
    Path ring; ring.addCentredArc(c.x, c.y, r, r, 0, a0, a0 + span, true);
    g.setColour(ui::dim.withAlpha(0.12f));
    g.strokePath(ring, PathStrokeType(3.f));
    const float frac = jlimit(0.f, 1.f, reduction / 12.f);
    if (frac > 0.002f)
    {
        Path v; v.addCentredArc(c.x, c.y, r, r, 0, a0, a0 + span * frac, true);
        g.setColour(ui::amber);
        g.strokePath(v, PathStrokeType(4.f, PathStrokeType::curved, PathStrokeType::rounded));
    }
}

// ------------------------------------------------------------ views
void SpectrumView::tick()
{
    using E = naturalair::DeEsserEngine;
    float peak = -200.f;
    for (int i = 0; i < E::kMeterPoints; ++i)
    {
        const float s = engine.getSpectrumDb(i);
        spec[(size_t) i] += (s - spec[(size_t) i]) * 0.35f;
        gain[(size_t) i] += (engine.getGainCurveDb(i) - gain[(size_t) i]) * 0.5f;
        peak = jmax(peak, spec[(size_t) i]);
    }
    specPeak += (peak - specPeak) * 0.1f;
    repaint();
}

void SpectrumView::paint(Graphics& g)
{
    using E = naturalair::DeEsserEngine;
    auto r = getLocalBounds().toFloat();
    g.setColour(ui::panel);
    g.fillRoundedRectangle(r, 8.f);
    auto a = r.reduced(10.f, 8.f);
    const float n = (float) (E::kMeterPoints - 1);
    auto X = [&](int i) { return a.getX() + a.getWidth() * (float) i / n; };

    // spectrum (relative to its own peak, 60 dB range)
    Path sp; sp.startNewSubPath(a.getX(), a.getBottom());
    for (int i = 0; i < E::kMeterPoints; ++i)
    {
        const float v = jlimit(0.f, 1.f, 1.f + (spec[(size_t) i] - specPeak) / 60.f);
        sp.lineTo(X(i), a.getBottom() - v * a.getHeight() * 0.85f);
    }
    sp.lineTo(a.getRight(), a.getBottom()); sp.closeSubPath();
    g.setColour(ui::dim.withAlpha(0.22f)); g.fillPath(sp);

    // gain curve: -15..+5 dB around a 0 dB line
    const float y0 = a.getY() + a.getHeight() * 0.75f, scale = a.getHeight() * 0.75f / 15.f;
    Path red, air; red.startNewSubPath(a.getX(), y0); air.startNewSubPath(a.getX(), y0);
    Path line;
    for (int i = 0; i < E::kMeterPoints; ++i)
    {
        const float gdb = jlimit(-15.f, 5.f, gain[(size_t) i]);
        const float y = y0 - gdb * scale;
        if (i == 0) line.startNewSubPath(X(i), y); else line.lineTo(X(i), y);
        red.lineTo(X(i), jmax(y, y0)); air.lineTo(X(i), jmin(y, y0));
    }
    red.lineTo(a.getRight(), y0); red.closeSubPath(); air.lineTo(a.getRight(), y0); air.closeSubPath();
    g.setColour(ui::amber.withAlpha(0.35f)); g.fillPath(red);
    g.setColour(ui::cyan.withAlpha(0.35f));  g.fillPath(air);
    g.setColour(ui::dim.withAlpha(0.4f)); g.drawHorizontalLine((int) y0, a.getX(), a.getRight());
    g.setColour(ui::text.withAlpha(0.9f)); g.strokePath(line, PathStrokeType(1.6f));

    // frequency labels
    g.setColour(ui::dim); g.setFont(Font(FontOptions(11.f)));
    for (float f : { 1000.f, 2000.f, 5000.f, 10000.f, 20000.f })
    {
        const float t = std::log(f / 500.f) / std::log(20000.f / 500.f);
        g.drawText(f >= 1000.f ? String((int) (f / 1000.f)) + "k" : String((int) f),
                   Rectangle<float>(a.getX() + a.getWidth() * t - 14.f, a.getBottom() - 12.f, 28.f, 12.f), Justification::centred);
    }
}

void HistoryStrip::push(float dB) { data[(size_t) pos] = dB; pos = (pos + 1) % (int) data.size(); repaint(); }

void HistoryStrip::paint(Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(ui::panel); g.fillRoundedRectangle(r, 6.f);
    auto a = r.reduced(6.f, 4.f);
    Path p; p.startNewSubPath(a.getX(), a.getY());
    const int n = (int) data.size();
    for (int i = 0; i < n; ++i)
    {
        const float v = jlimit(0.f, 1.f, data[(size_t) ((pos + i) % n)] / 12.f);
        p.lineTo(a.getX() + a.getWidth() * (float) i / (float) (n - 1), a.getY() + v * a.getHeight());
    }
    p.lineTo(a.getRight(), a.getY()); p.closeSubPath();
    g.setColour(ui::amber.withAlpha(0.75f)); g.fillPath(p);
}

void LevelBar::set(float lin)
{
    const float d = Decibels::gainToDecibels(lin, -90.f);
    db = d > db ? d : jmax(d, db - 2.5f);
    repaint();
}

void LevelBar::paint(Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(ui::panel); g.fillRoundedRectangle(r, 3.f);
    const float f = jlimit(0.f, 1.f, (db + 60.f) / 60.f);
    g.setColour(db > -1.f ? Colours::red : (db > -12.f ? ui::amber : ui::cyan));
    g.fillRoundedRectangle(r.withTrimmedTop(r.getHeight() * (1.f - f)), 3.f);
}

// ------------------------------------------------------------ editor
void NaturalAirEditor::setupKnob(Knob& k, const String& id, const String& title, const String& suffix)
{
    k.s.setSliderStyle(Slider::RotaryHorizontalVerticalDrag);
    k.s.setTextBoxStyle(Slider::TextBoxBelow, false, 72, 18);
    k.s.setMouseDragSensitivity(90);   // default 250 px per full turn; lower = each drag moves the value more
    k.s.setTextValueSuffix(suffix);
    k.s.setDoubleClickReturnValue(true, proc.apvts.getParameter(id)->convertFrom0to1(proc.apvts.getParameter(id)->getDefaultValue()));
    k.l.setText(title, dontSendNotification);
    k.l.setJustificationType(Justification::centred);
    k.l.setColour(Label::textColourId, ui::dim);
    k.l.setFont(Font(FontOptions(12.f)));
    addAndMakeVisible(k.s); addAndMakeVisible(k.l);
    k.a = std::make_unique<SA>(proc.apvts, id, k.s);
}

NaturalAirEditor::NaturalAirEditor(NaturalAirProcessor& p)
    : AudioProcessorEditor(&p), proc(p), spectrum(p.getEngine())
{
    setLookAndFeel(&laf);
    setResizable(true, true);
    setResizeLimits(585, 360, 1560, 960);
    getConstrainer()->setFixedAspectRatio(780.0 / 480.0);
    setSize(780, 480);

    addAndMakeVisible(spectrum); addAndMakeVisible(history);
    addAndMakeVisible(inBar); addAndMakeVisible(outBar);

    airKeep.setMouseDragSensitivity(90);
    airKeep.setDoubleClickReturnValue(true, 65.0);
    airKeep.setTextValueSuffix(" %");
    addAndMakeVisible(airKeep);
    airKeepA = std::make_unique<SA>(proc.apvts, "airKeep", airKeep);
    airKeepLabel.setText("AIR KEEP", dontSendNotification);
    airKeepLabel.setJustificationType(Justification::centred);
    airKeepLabel.setFont(Font(FontOptions(15.f, Font::bold)));
    addAndMakeVisible(airKeepLabel);

    setupKnob(threshold, "threshold", "THRESHOLD", " %");
    setupKnob(range, "range", "RANGE", " dB");
    setupKnob(width, "width", "WIDTH", " oct");
    setupKnob(precision, "precision", "PRECISION", " %");
    width.s.setNumDecimalPlacesToDisplay(2);
    setupKnob(attack, "attack", "ATTACK", " ms");
    setupKnob(release, "release", "RELEASE", " ms");
    setupKnob(freq, "freq", "FREQUENCY", " Hz");
    freq.s.onDragStart = [this]
    {
        if (freqAuto.getToggleState())      // touching the knob = you want manual control
            freqAuto.setToggleState(false, sendNotificationSync);
    };
    setupKnob(airBoost, "airBoost", "AIR BOOST", " dB");
    setupKnob(mix, "mix", "MIX", " %");
    setupKnob(input, "input", "INPUT", " dB");
    setupKnob(output, "output", "OUTPUT", " dB");

    listen.addItemList({ "Listen: Off", "Listen: Delta", "Listen: Detector" }, 1);
    addAndMakeVisible(listen);
    listenA = std::make_unique<CA>(proc.apvts, "listen", listen);

    for (auto* b : { &freqAuto, &adaptive, &bypass }) addAndMakeVisible(*b);
    freqAutoA = std::make_unique<BA>(proc.apvts, "freqAuto", freqAuto);
    adaptiveA = std::make_unique<BA>(proc.apvts, "adaptive", adaptive);
    bypassA = std::make_unique<BA>(proc.apvts, "bypass", bypass);

    for (auto* l : { &reductionLabel, &freqLabel })
    {
        l->setJustificationType(Justification::centred);
        l->setColour(Label::textColourId, ui::dim);
        l->setFont(Font(FontOptions(12.f)));
        addAndMakeVisible(*l);
    }
    startTimerHz(30);
}

NaturalAirEditor::~NaturalAirEditor() { stopTimer(); setLookAndFeel(nullptr); }

void NaturalAirEditor::paint(Graphics& g)
{
    g.fillAll(ui::bg);
    g.setColour(ui::text);
    g.setFont(Font(FontOptions(19.f, Font::bold)));
    g.drawText("NaturalAir", 18, 10, 120, 28, Justification::centredLeft);
    g.setColour(ui::cyan);
    g.drawText("De-Esser", 112, 10, 120, 28, Justification::centredLeft);
    // build tag: lets you check at a glance which build your DAW actually loaded (old VST3s are easy to keep by mistake)
    g.setColour(ui::dim);
    g.setFont(Font(FontOptions(11.f)));
    g.drawText("build 20", 200, 10, 80, 28, Justification::centredLeft);
}

void NaturalAirEditor::resized()
{
    const float s = (float) getWidth() / 780.f;
    auto R = [&](float x, float y, float w, float h) { return Rectangle<int>((int) (x * s), (int) (y * s), (int) (w * s), (int) (h * s)); };

    bypass.setBounds(R(560, 12, 90, 26));
    adaptive.setBounds(R(660, 12, 100, 26));
    inBar.setBounds(R(18, 52, 8, 236)); outBar.setBounds(R(754, 52, 8, 236));
    spectrum.setBounds(R(36, 48, 708, 116));

    airKeep.setBounds(R(280, 150, 220, 260));
    airKeepLabel.setBounds(R(280, 386, 220, 20));
    reductionLabel.setBounds(R(280, 408, 220, 18));

    auto place = [&](Knob& k, float x, float y)
    {
        k.l.setBounds(R(x, y, 110, 16)); k.s.setBounds(R(x, y + 14, 110, 112));
    };
    place(threshold, 40, 176); place(range, 40, 304);
    place(attack, 160, 176);   place(release, 160, 304);
    place(width, 495, 176); place(precision, 495, 304); place(freq, 610, 176);     place(airBoost, 610, 308 - 4);
    freqAuto.setBounds(R(660, 160, 56, 20));
    freqLabel.setBounds(R(560, 160, 90, 18));

    listen.setBounds(R(36, 436, 140, 26));
    mix.l.setBounds(R(190, 410, 60, 14)); mix.s.setBounds(R(190, 420, 70, 56));
    input.l.setBounds(R(640, 410, 60, 14)); input.s.setBounds(R(640, 420, 70, 56));
    output.l.setBounds(R(700, 410, 60, 14)); output.s.setBounds(R(700, 420, 70, 56));
    history.setBounds(R(280, 458, 220, 16));
    for (auto* k : { &mix, &input, &output }) k->s.setTextBoxStyle(Slider::TextBoxBelow, false, (int) (70 * s), (int) (14 * s));
}

void NaturalAirEditor::timerCallback()
{
    auto& e = proc.getEngine();
    const float red = e.getReductionDb();
    history.push(red);
    airKeep.setReduction(red);
    spectrum.tick();
    inBar.set(e.consumeInputPeak());
    outBar.set(e.consumeOutputPeak());
    reductionLabel.setText(String(-red, 1) + " dB", dontSendNotification);
    freq.s.setAlpha(freqAuto.getToggleState() ? 0.4f : 1.f);
    freqLabel.setText(freqAuto.getToggleState() ? "Auto " + String(e.getFrequencyHz() / 1000.f, 1) + " kHz" : String(), dontSendNotification);
}
