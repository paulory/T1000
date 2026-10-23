#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

// Third-party plugin hosting (VST3 / AU) for chain blocks of type PLUGIN.
//
// A hosted plugin is one more engine a chain block can carry, next to the
// NAM engine and the IR convolvers. It runs at the chain's base rate
// (kChainBaseSampleRate, 48 kHz) behind the block's own base-rate island,
// exactly like IR convolution: a plugin prepared once at 48 kHz never has to
// be re-prepared when the host rate or the oversampling factor moves, and it
// never sees the oversampled rate (most effects would behave differently or
// burn CPU there). The block's existing stages wrap it unchanged: input
// gain, EQ (pre or post), dry/wet mix, output gain, power fade, meters.
//
// Threading:
//   - Instances are created asynchronously on the message thread
//     (AudioPluginFormatManager::createPluginInstanceAsync), prepared there,
//     and only then spliced into the chain under the chain lock.
//   - process() is the only call made on the audio thread. It takes the
//     instance's callback lock like any JUCE host does, so plugins that
//     guard their own state with it (setStateInformation, parameter
//     changes from their editor) stay consistent.
//   - The editor window is owned here, so it can never outlive the instance.
//     Destruction off the message thread hands both to the message thread.

class HostedPlugin {
public:
  static constexpr double kSampleRate = 48000.0;  // == kChainBaseSampleRate (ChainDomain.h)

  explicit HostedPlugin(std::unique_ptr<juce::AudioPluginInstance> instance);
  ~HostedPlugin();

  // Configure buses (stereo in/out where the plugin allows it, else mono)
  // and prepare for `maxBlockFrames` base-rate frames per call. Allocates;
  // never on the audio thread while the block is live in a chain.
  void prepare(int maxBlockFrames);
  int preparedBlockSize() const noexcept { return preparedBlock; }

  // RT: run `numFrames` base-rate frames of the block's 1 or 2 working
  // channels through the plugin, in place. Longer runs are fed in prepared-
  // size chunks. A mono working buffer feeds every plugin input and takes
  // the average of a stereo output; a stereo buffer into a mono plugin is
  // summed to its single input and the output copied back to both sides.
  void process(float* const* channels, int numChannels, int numFrames) noexcept;

  juce::AudioPluginInstance& instance() noexcept { return *plugin; }
  const juce::PluginDescription& description() const noexcept { return desc; }

  juce::MemoryBlock getState() const;
  void setState(const juce::MemoryBlock& state);

  // Latency at the base rate, as the plugin reports it.
  int latencySamples() const;
  double tailSeconds() const;

  // Message thread. Opens (or brings to front) the plugin's own editor in a
  // floating window; a plugin without one gets JUCE's generic parameter
  // editor. Closing the window only hides it.
  void showEditor(const juce::String& title);
  void closeEditor();
  bool isEditorVisible() const;

private:
  class EditorWindow;

  void configureBuses();

  std::unique_ptr<juce::AudioPluginInstance> plugin;
  std::unique_ptr<EditorWindow> window;
  juce::PluginDescription desc;
  juce::AudioBuffer<float> scratch;
  juce::MidiBuffer midi;
  int preparedBlock = 0;
  int numIns = 0;   // main input bus channels
  int numOuts = 0;  // main output bus channels

  JUCE_DECLARE_NON_COPYABLE(HostedPlugin)
};

// Process-wide plugin catalog: the format manager (VST3 everywhere, AU on
// macOS) and the list of plugins the user has scanned or picked, persisted
// next to the plugin's settings file so every instance and every session
// shares it. Shared by all processor instances through
// juce::SharedResourcePointer.
class HostedPluginCatalog : private juce::ChangeListener {
public:
  HostedPluginCatalog();
  ~HostedPluginCatalog() override;

  // Where the known-plugin list lives. The first call loads it; later calls
  // with the same file are no-ops.
  void setStorageFile(const juce::File& file);

  juce::AudioPluginFormatManager& formats() noexcept { return formatManager; }
  juce::KnownPluginList& knownPlugins() noexcept { return known; }
  // Crash marker for the scanner: a plugin that takes the process down while
  // being scanned is blacklisted on the next scan instead of crashing again.
  juce::File deadMansPedalFile() const;

  // True when this build can host at least one format.
  bool hostingAvailable() const { return formatManager.getNumFormats() > 0; }

  // Every plugin type inside one file or bundle (a .vst3 / .component), also
  // recorded in the known list. Empty when nothing loadable is there.
  juce::Array<juce::PluginDescription> scanFile(const juce::File& file);

  // Stable text form of a description: what PLUGIN blocks persist in their
  // tone JSON (presets, DAW sessions, undo).
  static juce::String toXmlString(const juce::PluginDescription& description);
  static bool fromXmlString(const juce::String& xml, juce::PluginDescription& out);

  // Short user-facing format label ("VST3", "AU").
  static juce::String formatLabel(const juce::String& pluginFormatName);

private:
  void changeListenerCallback(juce::ChangeBroadcaster*) override;
  void save();

  juce::AudioPluginFormatManager formatManager;
  juce::KnownPluginList known;
  juce::File storageFile;
  bool loading = false;

  JUCE_DECLARE_NON_COPYABLE(HostedPluginCatalog)
};
