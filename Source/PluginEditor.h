#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

namespace ui
{
const juce::Colour bg      { 0xff121419 };
const juce::Colour panel   { 0xff1b1e25 };
const juce::Colour text    { 0xffe8eaee };
const juce::Colour dim     { 0xff8b919c };
const juce::Colour amber   { 0xffffa94d };   // reduction / harsh zone
const juce::Colour cyan    { 0xff56d6e8 };   // air / protected zone
}

class NaturalAirLookAndFeel : public juce::LookAndFeel_V4
{
public:
    NaturalAirLookAndFeel();
    void drawRotarySlider(juce::Graphics&, int x, int y, int w, int h, float pos,
                          float startAngle, float endAngle, juce::Slider&) override;
    void drawToggleButton(juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
};

// The protagonist: big knob with a live gain-reduction ring around it.
class AirKeepKnob : public juce::Slider
{
public:
    AirKeepKnob() { setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag); setTextBoxStyle(juce::Slider::TextBoxBelow, false, 90, 22); }
    void setReduction(float dB) { reduction = dB; repaint(); }
    void paintOverChildren(juce::Graphics&) override;
private:
    float reduction = 0.f;
};

class SpectrumView : public juce::Component
{
public:
    explicit SpectrumView(naturalair::DeEsserEngine& e) : engine(e) {}
    void paint(juce::Graphics&) override;
    void tick();
private:
    naturalair::DeEsserEngine& engine;
    std::array<float, naturalair::DeEsserEngine::kMeterPoints> spec {}, gain {};
    float specPeak = -100.f;
};

class HistoryStrip : public juce::Component
{
public:
    void push(float dB);
    void paint(juce::Graphics&) override;
private:
    std::array<float, 150> data {};
    int pos = 0;
};

class LevelBar : public juce::Component
{
public:
    void set(float linear);
    void paint(juce::Graphics&) override;
private:
    float db = -90.f;
};

class NaturalAirEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit NaturalAirEditor(NaturalAirProcessor&);
    ~NaturalAirEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    struct Knob { juce::Slider s; juce::Label l; std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> a; };
    void setupKnob(Knob&, const juce::String& id, const juce::String& title, const juce::String& suffix = {});

    NaturalAirProcessor& proc;
    NaturalAirLookAndFeel laf;

    SpectrumView spectrum;
    HistoryStrip history;
    LevelBar inBar, outBar;
    AirKeepKnob airKeep;
    juce::Label airKeepLabel, reductionLabel, freqLabel;
    Knob threshold, range, width, precision, attack, release, freq, airBoost, mix, input, output;
    juce::ComboBox listen;
    juce::ToggleButton freqAuto { "Auto" }, adaptive { "Adaptive" }, bypass { "Bypass" };

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    std::unique_ptr<SA> airKeepA;
    std::unique_ptr<BA> freqAutoA, adaptiveA, bypassA;
    std::unique_ptr<CA> listenA;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NaturalAirEditor)
};
