#include "HostedPlugin.h"

#include <algorithm>
#include <cmath>

// ##########################
// EDITOR WINDOW
// ##########################
// A plain floating window around the hosted plugin's editor. Owned by the
// HostedPlugin, so it is always torn down before the instance it shows.
// Closing only hides it and drops the editor (plugins free their GUI
// resources), reopening builds a fresh one.
class HostedPlugin::EditorWindow : public juce::DocumentWindow {
public:
  EditorWindow(const juce::String& title, juce::AudioPluginInstance& p)
      : juce::DocumentWindow(title, juce::Colour(0xff141416),
                             juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
        plugin(p) {
    setUsingNativeTitleBar(true);
    // Inside a DAW a plain top-level window drops behind the host's main
    // window on the first click there; plugin editors float above it.
    if (!juce::JUCEApplicationBase::isStandaloneApp())
      setAlwaysOnTop(true);
  }

  ~EditorWindow() override { clearContentComponent(); }

  void open() {
    if (getContentComponent() == nullptr) {
      juce::AudioProcessorEditor* editor =
          plugin.hasEditor() ? plugin.createEditorIfNeeded() : nullptr;
      if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor(plugin);
      setContentOwned(editor, true);
      setResizable(editor->isResizable(), false);
      if (!positioned) {
        centreWithSize(getWidth(), getHeight());
        positioned = true;
      }
    }
    setVisible(true);
    toFront(true);
  }

  void closeButtonPressed() override {
    setVisible(false);
    clearContentComponent();
  }

private:
  juce::AudioPluginInstance& plugin;
  bool positioned = false;
};

// ##########################
// HOSTED PLUGIN
// ##########################
HostedPlugin::HostedPlugin(std::unique_ptr<juce::AudioPluginInstance> instance)
    : plugin(std::move(instance)) {
  jassert(plugin != nullptr);
  desc = plugin->getPluginDescription();
}

HostedPlugin::~HostedPlugin() {
  // Plugin GUIs and most VST3/AU instances must be destroyed on the message
  // thread. Chain teardown normally happens there; a host restoring state
  // from another thread gets the work handed over instead.
  auto* mm = juce::MessageManager::getInstanceWithoutCreating();
  if (mm == nullptr || mm->isThisTheMessageThread()) {
    window.reset();
    if (plugin != nullptr)
      plugin->releaseResources();
    plugin.reset();
    return;
  }
  auto* w = window.release();
  auto* p = plugin.release();
  juce::MessageManager::callAsync([w, p] {
    delete w;
    if (p != nullptr)
      p->releaseResources();
    delete p;
  });
}

void HostedPlugin::configureBuses() {
  using Set = juce::AudioChannelSet;
  const auto original = plugin->getBusesLayout();

  // Main buses only: sidechains and aux outputs stay off (nothing feeds or
  // reads them in a serial chain). Stereo first, then the mono variants.
  const std::pair<Set, Set> candidates[] = {
      {Set::stereo(), Set::stereo()},
      {Set::mono(), Set::stereo()},
      {Set::mono(), Set::mono()},
  };
  for (const auto& [in, out] : candidates) {
    auto layout = original;
    if (!layout.inputBuses.isEmpty())
      layout.inputBuses.getReference(0) = in;
    if (!layout.outputBuses.isEmpty())
      layout.outputBuses.getReference(0) = out;
    for (int i = 1; i < layout.inputBuses.size(); ++i)
      layout.inputBuses.getReference(i) = Set::disabled();
    for (int i = 1; i < layout.outputBuses.size(); ++i)
      layout.outputBuses.getReference(i) = Set::disabled();
    if (plugin->checkBusesLayoutSupported(layout) && plugin->setBusesLayout(layout))
      return;
  }
  // Nothing matched: keep whatever the plugin chose for itself.
  plugin->setBusesLayout(original);
}

void HostedPlugin::prepare(int maxBlockFrames) {
  maxBlockFrames = std::max(1, maxBlockFrames);
  if (preparedBlock > 0)
    plugin->releaseResources();

  configureBuses();
  numIns = plugin->getMainBusNumInputChannels();
  numOuts = plugin->getMainBusNumOutputChannels();

  plugin->setRateAndBufferSizeDetails(kSampleRate, maxBlockFrames);
  plugin->setNonRealtime(false);
  plugin->prepareToPlay(kSampleRate, maxBlockFrames);

  const int work = std::max({2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels()});
  scratch.setSize(work, maxBlockFrames, false, true, true);
  scratch.clear();
  midi.ensureSize(512);
  preparedBlock = maxBlockFrames;
}

void HostedPlugin::process(float* const* channels, int numChannels, int numFrames) noexcept {
  if (plugin == nullptr || preparedBlock <= 0 || numChannels <= 0 || numFrames <= 0)
    return;

  const int work = scratch.getNumChannels();
  float* const* s = scratch.getArrayOfWritePointers();

  for (int start = 0; start < numFrames; start += preparedBlock) {
    const int n = std::min(preparedBlock, numFrames - start);

    // In: the working channels onto the plugin's main inputs.
    for (int ch = 0; ch < work; ++ch) {
      float* dst = s[ch];
      if (ch >= numIns) {
        std::fill(dst, dst + n, 0.0f);
      } else if (numIns == 1 && numChannels > 1) {
        const float* l = channels[0] + start;
        const float* r = channels[1] + start;
        for (int i = 0; i < n; ++i)
          dst[i] = 0.5f * (l[i] + r[i]);
      } else {
        const float* src = channels[std::min(ch, numChannels - 1)] + start;
        std::copy(src, src + n, dst);
      }
    }

    {
      // No allocation: AudioBuffer keeps up to 32 channel pointers inline.
      juce::AudioBuffer<float> view(s, work, n);
      midi.clear();
      const juce::ScopedLock callbackLock(plugin->getCallbackLock());
      if (plugin->isSuspended())
        continue;  // leave the working channels dry
      plugin->processBlock(view, midi);
    }

    if (numOuts <= 0)
      continue;  // an analyzer-style plugin: nothing comes back

    // Out: the plugin's main outputs back onto the working channels.
    if (numChannels == 1) {
      float* dst = channels[0] + start;
      if (numOuts >= 2) {
        for (int i = 0; i < n; ++i)
          dst[i] = 0.5f * (s[0][i] + s[1][i]);
      } else {
        std::copy(s[0], s[0] + n, dst);
      }
    } else {
      std::copy(s[0], s[0] + n, channels[0] + start);
      const float* right = numOuts >= 2 ? s[1] : s[0];
      std::copy(right, right + n, channels[1] + start);
    }
  }
}

juce::MemoryBlock HostedPlugin::getState() const {
  juce::MemoryBlock state;
  if (plugin != nullptr)
    plugin->getStateInformation(state);
  return state;
}

void HostedPlugin::setState(const juce::MemoryBlock& state) {
  if (plugin != nullptr && state.getSize() > 0)
    plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
}

int HostedPlugin::latencySamples() const {
  return plugin != nullptr ? std::max(0, plugin->getLatencySamples()) : 0;
}

double HostedPlugin::tailSeconds() const {
  if (plugin == nullptr)
    return 0.0;
  const double tail = plugin->getTailLengthSeconds();
  // "Infinite" tails (freeze reverbs) report inf; hosts cap them anyway.
  return std::isfinite(tail) ? juce::jlimit(0.0, 30.0, tail) : 30.0;
}

void HostedPlugin::showEditor(const juce::String& title) {
  JUCE_ASSERT_MESSAGE_THREAD
  if (plugin == nullptr)
    return;
  if (window == nullptr)
    window = std::make_unique<EditorWindow>(title, *plugin);
  window->open();
}

void HostedPlugin::closeEditor() {
  if (window != nullptr)
    window->closeButtonPressed();
}

bool HostedPlugin::isEditorVisible() const {
  return window != nullptr && window->isVisible();
}

// ##########################
// CATALOG
// ##########################
HostedPluginCatalog::HostedPluginCatalog() {
  // VST3 (and AU on macOS) when the build enables JUCE_PLUGINHOST_*; an
  // empty manager otherwise (tests, iOS), which disables hosting cleanly.
  juce::addDefaultFormatsToManager(formatManager);
  known.addChangeListener(this);
}

HostedPluginCatalog::~HostedPluginCatalog() {
  known.removeChangeListener(this);
}

void HostedPluginCatalog::setStorageFile(const juce::File& file) {
  if (file == storageFile)
    return;
  storageFile = file;
  // The first build scanned on a worker thread, which crashed on plugins
  // that must be created on the main thread; its crash marker would wrongly
  // blacklist them. Drop it (the v2 marker replaces it).
  const juce::File oldPedal = storageFile.getSiblingFile("hosted-plugins-scan.lock");
  const bool hadOldPedal = oldPedal.existsAsFile();
  oldPedal.deleteFile();
  if (auto xml = juce::parseXML(storageFile)) {
    const juce::ScopedValueSetter<bool> guard(loading, true);
    known.recreateFromXml(*xml);
  }
  if (hadOldPedal)
    known.clearBlacklistedFiles();  // saved via the change listener
}

juce::File HostedPluginCatalog::deadMansPedalFile() const {
  return storageFile == juce::File() ? juce::File()
                                     : storageFile.getSiblingFile("hosted-plugins-scan-v2.lock");
}

void HostedPluginCatalog::changeListenerCallback(juce::ChangeBroadcaster*) {
  if (!loading)
    save();
}

void HostedPluginCatalog::save() {
  if (storageFile == juce::File())
    return;
  if (auto xml = known.createXml()) {
    storageFile.getParentDirectory().createDirectory();
    xml->writeTo(storageFile);
  }
}

juce::Array<juce::PluginDescription> HostedPluginCatalog::scanFile(const juce::File& file) {
  juce::OwnedArray<juce::PluginDescription> found;
  const juce::String path = file.getFullPathName();
  for (auto* format : formatManager.getFormats())
    if (format->fileMightContainThisPluginType(path))
      known.scanAndAddFile(path, false, found, *format);

  juce::Array<juce::PluginDescription> out;
  for (auto* d : found)
    out.add(*d);
  return out;
}

juce::String HostedPluginCatalog::toXmlString(const juce::PluginDescription& description) {
  if (auto xml = description.createXml())
    return xml->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader());
  return {};
}

bool HostedPluginCatalog::fromXmlString(const juce::String& xml, juce::PluginDescription& out) {
  if (xml.isEmpty())
    return false;
  if (auto element = juce::parseXML(xml))
    return out.loadFromXml(*element);
  return false;
}

juce::String HostedPluginCatalog::formatLabel(const juce::String& pluginFormatName) {
  if (pluginFormatName.equalsIgnoreCase("AudioUnit"))
    return "AU";
  return pluginFormatName.isEmpty() ? juce::String("Plugin") : pluginFormatName.toUpperCase();
}
