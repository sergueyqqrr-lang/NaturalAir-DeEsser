#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "DSP/DeEsserEngine.h"

class NaturalAirProcessor : public juce::AudioProcessor
{
public:
    NaturalAirProcessor();
    ~NaturalAirProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "NaturalAir De-Esser"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    juce::AudioProcessorParameter* getBypassParameter() const override;

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    naturalair::DeEsserEngine& getEngine() { return engine; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    naturalair::Params readParams() const;

    naturalair::DeEsserEngine engine;
    std::atomic<float> *pAirKeep, *pThreshold, *pRange, *pWidth, *pPrecision, *pFreq, *pFreqAuto, *pAttack, *pRelease, *pAirBoost,
        *pMix, *pIn, *pOut, *pListen, *pAdaptive, *pBypass;
    int preparedChannels = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NaturalAirProcessor)
};
