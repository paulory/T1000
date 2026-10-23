// Hosted plugin (PLUGIN block) tests
//
// A PLUGIN block hosts a third-party VST3 / AU effect in the chain. These pin:
//   - persistence without an instance: a restored plugin block whose plugin
//     can't be instantiated (unknown description) still round-trips its
//     description and state blob through getStateInformation untouched, and
//     surfaces as a failed (retry) tile instead of vanishing,
//   - the UI summary (plugin flag, format badge, vendor),
//   - swaps and model switches are refused on plugin blocks.
// With a real VST3 at hand (T3K_TEST_VST3=/path/to/TestGain.vst3, a plain
// fixed-gain effect whose "gain" defaults to 0.5), also:
//   - the plugin loads into a slot, processes at 48 kHz, and halves the level,
//   - its latency is reported to the host,
//   - an oversampled chain still runs it at 48 kHz,
//   - its state rides the processor state into a fresh instance.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

namespace {

// Async instantiation runs on the message thread: pump it while waiting.
bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 10000) {
  const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
  while (juce::Time::getMillisecondCounter() < deadline) {
    if (done())
      return true;
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
  return done();
}

juce::var firstToneItem(TONE3000Processor& proc) {
  const juce::var state = proc.getChainState(-1);
  if (const auto* lane = state["chain"].getArray())
    for (const auto& item : *lane)
      if (item["kind"].toString() == "tone")
        return item;
  return {};
}

juce::ValueTree makePluginBlockTree(const juce::String& blockId, const juce::String& descriptionXml,
                                    const juce::String& stateBase64) {
  juce::DynamicObject::Ptr plugin = new juce::DynamicObject();
  plugin->setProperty("description", descriptionXml);
  plugin->setProperty("format", "VST3");
  plugin->setProperty("manufacturer", "Nobody");
  juce::DynamicObject::Ptr tone = new juce::DynamicObject();
  tone->setProperty("id", 0);
  tone->setProperty("title", "Ghost Plugin");
  tone->setProperty("format", "plugin");
  tone->setProperty("local", true);
  tone->setProperty("models", juce::Array<juce::var>());
  tone->setProperty("plugin", juce::var(plugin.get()));

  juce::ValueTree block("ChainBlock");
  block.setProperty("id", blockId, nullptr);
  block.setProperty("type", "plugin", nullptr);
  block.setProperty("enabled", true, nullptr);
  block.setProperty("inputGain", 0.5f, nullptr);
  block.setProperty("outputGain", 0.5f, nullptr);
  block.setProperty("mix", 1.0f, nullptr);
  block.setProperty("toneId", 0, nullptr);
  block.setProperty("toneJson", juce::JSON::toString(juce::var(tone.get())), nullptr);
  block.setProperty("activeModelId", 0, nullptr);
  block.setProperty("pluginState", stateBase64, nullptr);
  return block;
}

juce::ValueTree snapshotWith(const juce::ValueTree& block) {
  juce::ValueTree snapshot("ChainSnapshot");
  juce::ValueTree left("ChainBlocks");
  left.appendChild(block, nullptr);
  snapshot.appendChild(left, nullptr);
  return snapshot;
}

juce::ValueTree savedBlock(TONE3000Processor& proc, const juce::String& blockId) {
  juce::MemoryBlock data;
  proc.getStateInformation(data);
  const auto state = juce::ValueTree::readFromData(static_cast<const char*>(data.getData()) + 4,
                                                   data.getSize() - 4);
  const auto lane = state.getChildWithName("ChainSnapshot").getChildWithName("ChainBlocks");
  return lane.getChildWithProperty("id", blockId);
}

double rms(const std::vector<float>& x, size_t from) {
  double acc = 0.0;
  for (size_t i = from; i < x.size(); ++i)
    acc += static_cast<double>(x[i]) * x[i];
  return std::sqrt(acc / static_cast<double>(std::max<size_t>(1, x.size() - from)));
}

}  // namespace

TEST(HostedPluginTest, UnloadablePluginBlockKeepsItsStateAndShowsRetry) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);

  const juce::String stateB64 = juce::MemoryBlock("plugin-bytes", 12).toBase64Encoding();
  proc.restoreFromTree(snapshotWith(makePluginBlockTree("plug-1", "<not a description/>", stateB64)));

  ASSERT_TRUE(pumpUntil([&] { return static_cast<bool>(firstToneItem(proc)["loadFailed"]); }))
      << "a plugin that can't be instantiated must surface as a failed load";
  const juce::var item = firstToneItem(proc);
  EXPECT_EQ(item["blockId"].toString(), "plug-1");
  EXPECT_FALSE(static_cast<bool>(item["loaded"]));
  EXPECT_TRUE(static_cast<bool>(item["tone"]["plugin"]));
  EXPECT_EQ(item["tone"]["format"].toString(), "VST3");
  EXPECT_EQ(item["tone"]["title"].toString(), "Ghost Plugin");
  EXPECT_EQ(item["tone"]["user"]["username"].toString(), "Nobody");

  // Saved back out verbatim: description and state survive a session in
  // which the plugin was missing (another machine, uninstalled plugin).
  const auto saved = savedBlock(proc, "plug-1");
  ASSERT_TRUE(saved.isValid());
  EXPECT_EQ(saved.getProperty("type").toString(), "plugin");
  EXPECT_EQ(saved.getProperty("pluginState").toString(), stateB64);
  EXPECT_TRUE(saved.getProperty("toneJson").toString().contains("not a description"));

  // Plugin blocks can't become tones in place.
  EXPECT_FALSE(proc.swapTone("plug-1", "{\"id\":7,\"format\":\"ir\",\"models\":[{\"id\":1}]}"));
}

TEST(HostedPluginTest, Vst3LoadsProcessesAndPersists) {
  const juce::String path = juce::SystemStats::getEnvironmentVariable("T3K_TEST_VST3", {});
  if (path.isEmpty() || !juce::File(path).exists())
    GTEST_SKIP() << "set T3K_TEST_VST3 to a fixed-gain test VST3 to run";

  ChainTestProcessor proc;
  if (!proc.getHostedPluginCatalog().hostingAvailable())
    GTEST_SKIP() << "this build has no plugin formats";
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);

  const auto types = proc.getHostedPluginCatalog().scanFile(juce::File(path));
  ASSERT_FALSE(types.isEmpty()) << "scan found nothing in " << path;

  const juce::var res = proc.loadHostedPlugin(types.getReference(0));
  const std::string blockId = res["blockId"].toString().toStdString();
  ASSERT_FALSE(blockId.empty()) << res["error"].toString();
  ASSERT_TRUE(pumpUntil([&] { return static_cast<bool>(firstToneItem(proc)["loaded"]); }))
      << "plugin never finished instantiating";

  // Latency (64 base-rate samples) reaches the host once the async report runs.
  EXPECT_TRUE(pumpUntil([&] { return proc.getLatencySamples() >= 64; }, 2000));

  // Level: a 1 kHz sine through the 0.5 gain plugin vs the same chain bypassed.
  const auto sine = makeSine(48000, 1000.0, 0.25f);
  const double wet = rms(processStereo(proc, sine).first, 24000);
  ASSERT_TRUE(proc.setBlockParam(blockId, "enabled", 0.0));
  const double dry = rms(processStereo(proc, sine).first, 24000);
  ASSERT_GT(dry, 0.01);
  EXPECT_NEAR(wet / dry, 0.5, 0.02) << "wet " << wet << " dry " << dry;
  ASSERT_TRUE(proc.setBlockParam(blockId, "enabled", 1.0));

  // Oversampled chain (x4): the plugin keeps running at 48 kHz inside its
  // base-rate island (the test plugin outputs silence at any other rate).
  proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(1.0f);
  proc.parameters.getParameter("osFactor")->setValueNotifyingHost(0.5f);  // 4x
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  ASSERT_TRUE(pumpUntil([&] { return !proc.isChainEditFadeHeld(); }));
  const double wetOs = rms(processStereo(proc, sine).first, 24000);
  EXPECT_NEAR(wetOs / dry, 0.5, 0.03) << "oversampled wet " << wetOs;
  proc.parameters.getParameter("osEnabled")->setValueNotifyingHost(0.0f);
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
  ASSERT_TRUE(pumpUntil([&] { return !proc.isChainEditFadeHeld(); }));

  // State round trip into a fresh processor (project reopen).
  juce::MemoryBlock data;
  proc.getStateInformation(data);
  ChainTestProcessor reopened;
  reopened.setPlayConfigDetails(2, 2, kFs, 512);
  reopened.prepareToPlay(kFs, 512);
  reopened.setStateInformation(data.getData(), static_cast<int>(data.getSize()));
  ASSERT_TRUE(pumpUntil([&] {
    return static_cast<bool>(firstToneItem(reopened)["loaded"]) && !reopened.isChainEditFadeHeld();
  }));
  EXPECT_EQ(savedBlock(reopened, juce::String(blockId)).getProperty("pluginState").toString(),
            savedBlock(proc, juce::String(blockId)).getProperty("pluginState").toString());
  const double reopenedWet = rms(processStereo(reopened, sine).first, 24000);
  EXPECT_NEAR(reopenedWet / dry, 0.5, 0.02);

  // Remove, then undo: the block comes back as a fresh instance restored
  // from the state captured in the undo snapshot, and plays the same.
  letAudioGoIdle();
  ASSERT_TRUE(proc.removeChainBlock(blockId));
  EXPECT_FALSE(firstToneItem(proc).isObject());
  ASSERT_TRUE(proc.undoChain());
  ASSERT_TRUE(pumpUntil([&] {
    return static_cast<bool>(firstToneItem(proc)["loaded"]) && !proc.isChainEditFadeHeld();
  }));
  EXPECT_EQ(firstToneItem(proc)["blockId"].toString(), juce::String(blockId));
  EXPECT_NEAR(rms(processStereo(proc, sine).first, 24000) / dry, 0.5, 0.02);
}
