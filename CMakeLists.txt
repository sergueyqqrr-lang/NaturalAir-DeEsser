cmake_minimum_required(VERSION 3.22)
project(NaturalAirDeEsser VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(NA_BUILD_PLUGIN "Build the JUCE plugin (VST3/AU/AAX)" ON)
option(NA_BUILD_TESTS  "Build the standalone DSP engine tests" ON)
option(NA_ENABLE_AAX   "Build AAX (needs AAX_SDK_PATH, Avid/PACE tooling)" OFF)
set(AAX_SDK_PATH "" CACHE PATH "Path to the Avid AAX SDK")

# ---------------------------------------------------------------- DSP engine tests
# Threading setup suggested by the CI diagnosis (harmless; the engine itself uses no threads).
if(WIN32)
  set(CMAKE_USE_WIN32_THREADS_INIT ON)
  set(THREADS_PREFER_PTHREAD_FLAG OFF)
else()
  set(THREADS_PREFER_PTHREAD_FLAG ON)
endif()
find_package(Threads REQUIRED)

if(NA_BUILD_TESTS)
  enable_testing()
  add_executable(engine_test Tests/engine_test.cpp)
  target_compile_features(engine_test PRIVATE cxx_std_17)
  target_link_libraries(engine_test PRIVATE Threads::Threads)
  add_executable(process_wav Tools/process_wav.cpp)   # offline wav tool for calibration
  target_compile_features(process_wav PRIVATE cxx_std_17)
  add_test(NAME engine_test COMMAND engine_test)
endif()

# ---------------------------------------------------------------- Plugin
if(NA_BUILD_PLUGIN)
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.6
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(JUCE)

  set(NA_FORMATS VST3)
  if(APPLE)
    list(APPEND NA_FORMATS AU)
  endif()
  if(NA_ENABLE_AAX)
    juce_set_aax_sdk_path("${AAX_SDK_PATH}")
    list(APPEND NA_FORMATS AAX)
  endif()
  if(NOT APPLE AND NOT WIN32)
    list(APPEND NA_FORMATS Standalone)
  endif()

  juce_add_plugin(NaturalAirDeEsser
    COMPANY_NAME                "NaturalAir"
    PLUGIN_MANUFACTURER_CODE    Nair
    PLUGIN_CODE                 Nade
    FORMATS                     ${NA_FORMATS}
    PRODUCT_NAME                "NaturalAir De-Esser"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Dynamics
    AAX_CATEGORY                AAX_ePlugInCategory_Dynamics
    AU_MAIN_TYPE                kAudioUnitType_Effect)

  target_sources(NaturalAirDeEsser PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

  target_include_directories(NaturalAirDeEsser PRIVATE Source)

  target_compile_definitions(NaturalAirDeEsser PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

  target_link_libraries(NaturalAirDeEsser
    PRIVATE
      juce::juce_audio_utils
    PUBLIC
      juce::juce_recommended_config_flags
      juce::juce_recommended_lto_flags
      juce::juce_recommended_warning_flags)
endif()
