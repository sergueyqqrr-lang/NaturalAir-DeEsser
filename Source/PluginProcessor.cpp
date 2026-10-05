#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

AudioProcessorValueTreeState::ParameterLayout NaturalAirProcessor::createLayout()
{
    std::vector<std::unique_ptr<RangedAudioParameter>> p;
    auto F = [](const char* id, const char* name, NormalisableRange<float> r, float def, const char* unit)
    {
        return std::make_unique<AudioParameterFloat>(ParameterID { id, 1 }, name, r, def,
                                                     AudioParameterFloatAttributes().withLabel(unit));
    };
    p.push_back(F("airKeep",   "Air Keep",  { 0.f, 100.f, 0.1f }, 65.f, "%"));
    p.push_back(F("threshold", "Threshold", { 0.f, 100.f, 0.1f }, 50.f, "%"));
    p.push_back(F("range",     "Range",     { 0.f, 30.f, 0.1f },  12.f, "dB"));
    p.push_back(F("precision", "Precision", { 0.f, 100.f, 0.1f }, 75.f, "%"));
    p.push_back(F("width",     "Width",     { 0.15f, 1.6f, 0.01f }, 0.40f, "oct"));
    p.push_back(F("freq",      "Frequency", { 3500.f, 12000.f, 1.f, 0.5f }, 7000.f, "Hz"));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { "freqAuto", 1 }, "Auto Frequency", true));
    auto logRange = [](float lo, float hi, float interval, float centre)
    {
        NormalisableRange<float> r(lo, hi, interval);
        r.setSkewForCentre(centre);
        return r;
    };
    p.push_back(F("attack",  "Attack",  logRange(0.01f, 200.f, 0.01f, 1.5f),   0.5f, "ms"));
    p.push_back(F("release", "Release", logRange(0.5f, 3000.f, 0.1f, 60.f),   50.f, "ms"));
    p.push_back(F("airBoost",  "Air Boost", { 0.f, 3.f, 0.1f },   0.f,  "dB"));
    p.push_back(F("mix",       "Mix",       { 0.f, 100.f, 0.1f }, 100.f, "%"));
    p.push_back(F("input",     "Input",     { -18.f, 18.f, 0.1f }, 0.f, "dB"));
    p.push_back(F("output",    "Output",    { -18.f, 18.f, 0.1f }, 0.f, "dB"));
    p.push_back(std::make_unique<AudioParameterChoice>(ParameterID { "listen", 1 }, "Listen",
                                                       StringArray { "Off", "Delta", "Detector" }, 0));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { "adaptive", 1 }, "Adaptive", true));
    p.push_back(std::make_unique<AudioParameterBool>(ParameterID { "bypass", 1 }, "Bypass", false));
    return { p.begin(), p.end() };
}

NaturalAirProcessor::NaturalAirProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", AudioChannelSet::stereo(), true)
                         .withOutput("Output", AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "NaturalAirState", createLayout())
{
    pAirKeep = apvts.getRawParameterValue("airKeep");
    pThreshold = apvts.getRawParameterValue("threshold");
    pRange = apvts.getRawParameterValue("range");
    pWidth = apvts.getRawParameterValue("width");
    pPrecision = apvts.getRawParameterValue("precision");
    pFreq = apvts.getRawParameterValue("freq");
    pFreqAuto = apvts.getRawParameterValue("freqAuto");
    pAttack = apvts.getRawParameterValue("attack");
    pRelease = apvts.getRawParameterValue("release");
    pAirBoost = apvts.getRawParameterValue("airBoost");
    pMix = apvts.getRawParameterValue("mix");
    pIn = apvts.getRawParameterValue("input");
    pOut = apvts.getRawParameterValue("output");
    pListen = apvts.getRawParameterValue("listen");
    pAdaptive = apvts.getRawParameterValue("adaptive");
    pBypass = apvts.getRawParameterValue("bypass");
}

AudioProcessorParameter* NaturalAirProcessor::getBypassParameter() const
{
    return apvts.getParameter("bypass");
}

bool NaturalAirProcessor::isBusesLayoutSupported(const BusesLayout& l) const
{
    const auto in = l.getMainInputChannelSet(), out = l.getMainOutputChannelSet();
    return in == out && (in == AudioChannelSet::mono() || in == AudioChannelSet::stereo());
}

naturalair::Params NaturalAirProcessor::readParams() const
{
    naturalair::Params p;
    p.airKeep = pAirKeep->load() * 0.01f;
    p.threshold = pThreshold->load() * 0.01f;
    p.rangeDb = pRange->load();
    p.widthOct = pWidth->load();
    p.precision = pPrecision->load() * 0.01f;
    p.freqHz = pFreq->load();
    p.freqAuto = pFreqAuto->load() > 0.5f;
    p.attackMs = pAttack->load();
    p.releaseMs = pRelease->load();
    p.airBoostDb = pAirBoost->load();
    p.mix = pMix->load() * 0.01f;
    p.inputDb = pIn->load();
    p.outputDb = pOut->load();
    p.listen = (int) std::lround(pListen->load());
    p.adaptive = pAdaptive->load() > 0.5f;
    p.bypass = pBypass->load() > 0.5f;
    return p;
}

void NaturalAirProcessor::prepareToPlay(double sampleRate, int)
{
    preparedChannels = std::max(1, std::min(2, getTotalNumInputChannels()));
    engine.prepare(sampleRate, preparedChannels);
    engine.setParams(readParams());
    engine.reset();
    setLatencySamples(engine.getLatencySamples());   // fixed per sample rate
}

void NaturalAirProcessor::processBlock(AudioBuffer<float>& buffer, MidiBuffer&)
{
    ScopedNoDenormals noDenormals;
    const int nCh = std::min(buffer.getNumChannels(), preparedChannels);
    for (int c = nCh; c < buffer.getNumChannels(); ++c) buffer.clear(c, 0, buffer.getNumSamples());
    engine.setParams(readParams());
    engine.process(buffer.getArrayOfWritePointers(), buffer.getNumSamples());
}

void NaturalAirProcessor::getStateInformation(MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty("stateVersion", 1, nullptr);
    if (auto xml = state.createXml()) copyXmlToBinary(*xml, dest);
}

void NaturalAirProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType()))
            apvts.replaceState(ValueTree::fromXml(*xml));
}

AudioProcessorEditor* NaturalAirProcessor::createEditor() { return new NaturalAirEditor(*this); }

AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new NaturalAirProcessor(); }
