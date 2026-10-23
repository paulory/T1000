// Hosted VST3 / AU plugins from the UI side: the insert slot's "Load
// Plugin" menu (scanned plugins grouped by vendor, plus Browse… and the
// scanner), the plugin manager window (JUCE's PluginListComponent over the
// shared catalog) and the plugin block's editor window. Errors surface as a
// toast. Everything is inert when the backend can't host (available()).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>
#include <string>

#include "ChainStore.h"
#include "Toast.h"
#include "backend/Backend.h"

namespace t3k::ui {

class HostedPlugins {
public:
  HostedPlugins(Backend& backend, ChainStore& chain, Toast& toast);
  ~HostedPlugins();

  bool available() const { return backend_.pluginFormats() != nullptr; }

  // The plugin picker for the insert slot `targetInsertId`, opened under
  // `anchor`. Picking a plugin adds it to that slot.
  void pick(const std::string& targetInsertId, juce::Component& anchor);
  // OS file picker for a .vst3 / .component bundle, loaded into the slot.
  void browse(const std::string& targetInsertId);
  // The scan / manage window (one at a time).
  void openManager();
  // The plugin block's own editor window.
  void openEditor(const std::string& blockId);
  // The plugin's icon: one in its own bundle, else the vendor's standalone
  // app of the same name (macOS). Invalid when there is none. Cached.
  juce::Image iconFor(const juce::String& pluginPath, const juce::String& name,
                      const juce::String& vendor);

private:
  class ManagerWindow;

  void load(const juce::PluginDescription& description, const std::string& targetInsertId);
  void loadFromFile(const juce::File& file, const std::string& targetInsertId);
  juce::Array<juce::PluginDescription> effectTypes() const;

  Backend& backend_;
  ChainStore& chain_;
  Toast& toast_;
  std::unique_ptr<juce::FileChooser> chooser_;
  std::unique_ptr<ManagerWindow> manager_;
  std::map<juce::String, juce::Image> icons_;

  JUCE_DECLARE_WEAK_REFERENCEABLE(HostedPlugins)
};

}  // namespace t3k::ui
