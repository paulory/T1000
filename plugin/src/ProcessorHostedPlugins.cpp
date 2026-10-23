#include "Processor.h"

// ##############################
// HOSTED PLUGINS (VST3 / AU blocks)
// ##############################
// See loadHostedPlugin in Processor.h and HostedPlugin.h. A PLUGIN block is
// an ordinary chain block whose "tone" is a plugin description: everything
// that moves blocks around (reorder, cross-lane moves, duplicate, copy /
// paste, undo / redo, presets, DAW state) already works on blocks, so the
// plugin-specific surface is just creation, instantiation and the editor.

namespace {

// The tone JSON a PLUGIN block persists. `plugin.description` is the full
// PluginDescription as XML (what instantiation needs); the rest is what the
// UI shows. `local` keeps every catalog-only path (metadata refresh, model
// paging) away from it.
juce::var makePluginToneVar(const juce::PluginDescription& d) {
  juce::DynamicObject::Ptr plugin = new juce::DynamicObject();
  plugin->setProperty("description", HostedPluginCatalog::toXmlString(d));
  plugin->setProperty("format", HostedPluginCatalog::formatLabel(d.pluginFormatName));
  plugin->setProperty("manufacturer", d.manufacturerName);
  plugin->setProperty("identifier", d.createIdentifierString());

  juce::DynamicObject::Ptr tone = new juce::DynamicObject();
  tone->setProperty("id", 0);
  tone->setProperty("title", d.name.isNotEmpty() ? d.name : d.descriptiveName);
  tone->setProperty("format", "plugin");
  tone->setProperty("gear", "plugin");
  tone->setProperty("local", true);
  tone->setProperty("models", juce::Array<juce::var>());
  tone->setProperty("plugin", juce::var(plugin.get()));
  return juce::var(tone.get());
}

juce::var errorResult(const juce::String& message) {
  juce::DynamicObject::Ptr out = new juce::DynamicObject();
  out->setProperty("error", message);
  return juce::var(out.get());
}

}  // namespace

juce::var TONE3000Processor::loadHostedPlugin(const juce::PluginDescription& description,
                                              const std::string& targetInsertId) {
  JUCE_ASSERT_MESSAGE_THREAD
  if (!hostedCatalog->hostingAvailable())
    return errorResult("Plugin hosting isn't available in this build.");
  if (description.isInstrument)
    return errorResult(description.name + " is an instrument; only effect plugins fit in the chain.");

  const juce::var toneVar = makePluginToneVar(description);
  const juce::String toneJson = juce::JSON::toString(toneVar);
  const std::string blockId = juce::Uuid().toString().toStdString();

  {
    juce::ScopedLock lock(chainMutex);
    pushChainHistory();

    auto block = std::make_unique<ChainBlock>(blockId, ChainBlockType::PLUGIN);
    setToneOnBlock(*block, 0, toneJson, toneVar);
    block->activeModelId = 0;
    block->mixNormalized = 1.0f;  // an insert effect: fully wet, the plugin has its own mix
    // Like loadTone, the one creation path that skips applyBlockSettings.
    prepareBlockForChainRate(*block);
    landBlockInInsertSlot(std::move(block), targetInsertId);

    if (ChainBlock* landed = findBlockById(blockId))
      queueHostedPluginLoad(*landed);
    bumpChainRevision();
  }

  juce::Logger::writeToLog("[HostedPlugin] Loading " + description.pluginFormatName + " '" +
                           description.name + "' into block " + juce::String(blockId));
  juce::DynamicObject::Ptr out = new juce::DynamicObject();
  out->setProperty("blockId", juce::String(blockId));
  return juce::var(out.get());
}

void TONE3000Processor::queueHostedPluginLoad(ChainBlock& block) {
  block.loaded = false;
  block.loadFailed = false;
  block.modelLoading = true;
  const juce::uint32 token = ++block.hostedLoadToken;

  juce::PluginDescription description;
  if (!HostedPluginCatalog::fromXmlString(block.toneVar["plugin"]["description"].toString(),
                                          description)) {
    juce::Logger::writeToLog("[HostedPlugin] Block " + juce::String(block.id) +
                             " carries no readable plugin description");
    block.loadFailed = true;
    block.modelLoading = false;
    return;
  }

  // Instantiation must happen on the message thread (VST3 and AU both
  // require it) and outside chainMutex (it can take a while), so it is
  // always posted, even when the caller already is the message thread.
  auto alive = aliveFlag;
  juce::MessageManager::callAsync([this, alive, blockId = block.id, token, description,
                                   state = block.hostedPluginState] {
    if (!alive->load())
      return;
    const int blockSize = juce::jmax(1, chainBaseBlockSize());
    hostedCatalog->formats().createPluginInstanceAsync(
        description, HostedPlugin::kSampleRate, blockSize,
        [this, alive, blockId, token, state](std::unique_ptr<juce::AudioPluginInstance> instance,
                                             const juce::String& error) {
          if (!alive->load())
            return;  // the instance dies here, on the message thread
          finishHostedPluginLoad(blockId, token, std::move(instance), error, state);
        });
  });
}

void TONE3000Processor::finishHostedPluginLoad(const std::string& blockId, juce::uint32 token,
                                               std::unique_ptr<juce::AudioPluginInstance> instance,
                                               const juce::String& error,
                                               const juce::MemoryBlock& state) {
  JUCE_ASSERT_MESSAGE_THREAD

  if (instance == nullptr) {
    juce::Logger::writeToLog("[HostedPlugin] Instantiation failed for block " +
                             juce::String(blockId) + ": " +
                             (error.isNotEmpty() ? error : juce::String("unknown error")));
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block != nullptr && block->type == ChainBlockType::PLUGIN && block->hostedLoadToken == token) {
      block->loaded = false;
      block->loadFailed = true;
      block->modelLoading = false;
      bumpChainRevision();
    }
    return;
  }

  // Prepared off the chain lock; the audio thread never sees it until the
  // splice below. Declared before the lock so an instance that turns out to
  // be stale is destroyed after the lock is released.
  auto hosted = std::make_unique<HostedPlugin>(std::move(instance));
  hosted->prepare(juce::jmax(1, chainBaseBlockSize()));
  hosted->setState(state);

  std::unique_ptr<HostedPlugin> previous;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type != ChainBlockType::PLUGIN ||
        block->hostedLoadToken != token) {
      juce::Logger::writeToLog("[HostedPlugin] Dropping stale instance for block " +
                               juce::String(blockId));
      return;
    }

    // The block size may have moved while the instance was being built.
    const int blockSize = juce::jmax(1, chainBaseBlockSize());
    if (hosted->preparedBlockSize() != blockSize)
      hosted->prepare(blockSize);
    block->hostedBaseRateIsland.prepare(chainOversampleFactor.load(), blockSize);

    // Per-block smoothers, as applyPreparedModelToChainBlock arms them for
    // tone engines: prepareChain only covered blocks present at
    // prepareToPlay. The wet path enters from bypass (fresh instance).
    const double rate = chainSampleRate();
    block->inputGainSmoother.reset(rate, 0.05f);
    block->outputGainSmoother.reset(rate, 0.05f);
    block->mixSmoother.reset(rate, 0.05f);
    block->mixSmoother.setCurrentAndTargetValue(block->mixNormalized);
    block->swapFadePending.store(false);
    block->swapFadeDone.store(false);
    block->swapMuteWet.store(false);
    block->swapWetMuteGain.reset(rate, kWetFadeSeconds);
    block->swapWetMuteGain.setCurrentAndTargetValue(1.0f);
    block->wetFadeGain.reset(rate, kWetFadeSeconds);
    block->wetFadeGain.setCurrentAndTargetValue(0.0f);

    previous = std::move(block->hosted);
    block->hosted = std::move(hosted);
    block->loaded = true;
    block->loadFailed = false;
    block->modelLoading = false;
    // `loaded` going true is what lets the wet fade glide the plugin in
    // from bypass (see processChainOnBuffer), so no click on arrival.
    refreshIrTailLength();
    bumpChainRevision();
  }
  juce::Logger::writeToLog("[HostedPlugin] Block " + juce::String(blockId) + " ready");
}

bool TONE3000Processor::openHostedPluginEditor(const std::string& blockId) {
  JUCE_ASSERT_MESSAGE_THREAD
  HostedPlugin* hosted = nullptr;
  juce::String title;
  {
    juce::ScopedLock lock(chainMutex);
    ChainBlock* block = findBlockById(blockId);
    if (block == nullptr || block->type != ChainBlockType::PLUGIN || block->hosted == nullptr)
      return false;
    hosted = block->hosted.get();
    title = block->toneVar["title"].toString();
  }
  // Outside the lock (editors can take a moment to build). Blocks and their
  // instances are only destroyed on the message thread, which is this one,
  // so the pointer stays valid for the call.
  hosted->showEditor(title);
  return true;
}
