#include "HostedPlugins.h"

namespace t3k::ui {

namespace {

// Ids for the picker's own rows; the plugin rows use KnownPluginList's
// range (addToMenu / getIndexChosenByMenu), far above these.
constexpr int kBrowseId = 1;
constexpr int kManageId = 2;

juce::File defaultPluginFolder() {
#if JUCE_MAC
  return juce::File("/Library/Audio/Plug-Ins/VST3");
#elif JUCE_WINDOWS
  return juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory)
      .getChildFile("Common Files/VST3");
#else
  return juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".vst3");
#endif
}

}  // namespace

// The plugin manager: JUCE's scanner/list over the shared catalog, in a
// floating window. Closing it deletes it.
class HostedPlugins::ManagerWindow : public juce::DocumentWindow {
public:
  ManagerWindow(HostedPlugins& owner, juce::AudioPluginFormatManager& formats,
                juce::KnownPluginList& list, const juce::File& deadMansPedal)
      : juce::DocumentWindow("Plugins", juce::Colour(0xff141416), juce::DocumentWindow::closeButton),
        owner_(owner) {
    setUsingNativeTitleBar(true);
    // Scan on the message thread (no async-only plugins, no scanner
    // thread). Many plugins (Neural DSP's among them) are only safe to
    // create and destroy on the main thread; a threaded scan crashed the
    // app while probing them.
    auto* content = new juce::PluginListComponent(formats, list, deadMansPedal, nullptr,
                                                  /*allowAsync=*/false);
    content->setNumberOfThreadsForScanning(0);
    content->setSize(640, 480);
    setContentOwned(content, true);
    setResizable(true, false);
    setResizeLimits(420, 300, 2000, 2000);
    if (!juce::JUCEApplicationBase::isStandaloneApp()) setAlwaysOnTop(true);
    centreWithSize(getWidth(), getHeight());
    setVisible(true);
  }

  void closeButtonPressed() override {
    juce::WeakReference<HostedPlugins> owner(&owner_);
    // Deleting ourselves from inside our own callback is not allowed.
    juce::MessageManager::callAsync([owner] {
      if (owner != nullptr) owner->manager_.reset();
    });
    setVisible(false);
  }

private:
  HostedPlugins& owner_;
};

HostedPlugins::HostedPlugins(Backend& backend, ChainStore& chain, Toast& toast)
    : backend_(backend), chain_(chain), toast_(toast) {}

HostedPlugins::~HostedPlugins() = default;

juce::Array<juce::PluginDescription> HostedPlugins::effectTypes() const {
  juce::Array<juce::PluginDescription> types;
  if (auto* list = backend_.knownPlugins())
    for (const auto& d : list->getTypes())
      if (!d.isInstrument) types.add(d);
  return types;
}

void HostedPlugins::pick(const std::string& targetInsertId, juce::Component& anchor) {
  if (!available()) {
    toast_.show("Plugin hosting isn't available here");
    return;
  }

  const auto types = effectTypes();
  juce::PopupMenu menu;
  if (types.isEmpty()) {
    menu.addSectionHeader("No plugins scanned yet");
  } else {
    menu.addSectionHeader("Add Plugin");
    juce::KnownPluginList::addToMenu(menu, types, juce::KnownPluginList::sortByManufacturer);
  }
  menu.addSeparator();
  menu.addItem(kBrowseId, "Browse for Plugin File...");
  menu.addItem(kManageId, types.isEmpty() ? "Scan for Plugins..." : "Manage Plugins...");

  juce::WeakReference<HostedPlugins> self(this);
  menu.showMenuAsync(
      juce::PopupMenu::Options().withTargetComponent(&anchor).withParentComponent(nullptr),
      [self, types, target = targetInsertId](int result) {
        if (self == nullptr || result == 0) return;
        if (result == kBrowseId) {
          self->browse(target);
          return;
        }
        if (result == kManageId) {
          self->openManager();
          return;
        }
        const int index = juce::KnownPluginList::getIndexChosenByMenu(types, result);
        if (juce::isPositiveAndBelow(index, types.size())) self->load(types.getReference(index), target);
      });
}

void HostedPlugins::browse(const std::string& targetInsertId) {
  if (chooser_ != nullptr || !available()) return;
  chooser_ = std::make_unique<juce::FileChooser>("Load Plugin", defaultPluginFolder(),
#if JUCE_MAC
                                                 juce::String("*.vst3;*.component")
#else
                                                 juce::String("*.vst3")
#endif
  );
  // Plugin bundles are folders on disk; let the dialog pick either.
  const int flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                    juce::FileBrowserComponent::canSelectDirectories;

  juce::WeakReference<HostedPlugins> self(this);
  chooser_->launchAsync(flags, [self, target = targetInsertId](const juce::FileChooser& chooser) {
    if (self == nullptr) return;
    juce::MessageManager::callAsync([self] {
      if (self != nullptr) self->chooser_.reset();
    });
    const auto results = chooser.getResults();
    if (!results.isEmpty()) self->loadFromFile(results.getReference(0), target);
  });
}

void HostedPlugins::loadFromFile(const juce::File& file, const std::string& targetInsertId) {
  juce::Array<juce::PluginDescription> effects;
  for (const auto& d : backend_.scanPluginFile(file))
    if (!d.isInstrument) effects.add(d);

  if (effects.isEmpty()) {
    toast_.show("No effect plugin found in " + file.getFileName());
    return;
  }
  if (effects.size() == 1) {
    load(effects.getReference(0), targetInsertId);
    return;
  }

  // A bundle with several plugins (a suite, or VST3 + its variants): ask.
  juce::PopupMenu menu;
  menu.addSectionHeader(file.getFileNameWithoutExtension());
  for (int i = 0; i < effects.size(); ++i) menu.addItem(i + 1, effects.getReference(i).name);
  juce::WeakReference<HostedPlugins> self(this);
  menu.showMenuAsync(juce::PopupMenu::Options(), [self, effects, targetInsertId](int result) {
    if (self != nullptr && juce::isPositiveAndBelow(result - 1, effects.size()))
      self->load(effects.getReference(result - 1), targetInsertId);
  });
}

void HostedPlugins::load(const juce::PluginDescription& description, const std::string& targetInsertId) {
  const auto error = chain_.loadHostedPlugin(description, targetInsertId);
  if (error.isNotEmpty()) toast_.show(error);
}

void HostedPlugins::openManager() {
  if (!available()) return;
  if (manager_ != nullptr) {
    manager_->toFront(true);
    return;
  }
  manager_ = std::make_unique<ManagerWindow>(*this, *backend_.pluginFormats(), *backend_.knownPlugins(),
                                             backend_.pluginScanDeadMansPedal());
}

void HostedPlugins::openEditor(const std::string& blockId) {
  if (!backend_.openHostedPluginEditor(blockId)) toast_.show("The plugin is still loading");
}

}  // namespace t3k::ui
