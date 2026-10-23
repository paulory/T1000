#include "ToneTile.h"

#include "GalleryGeometry.h"
#include "core/Fonts.h"
#include "core/Help.h"
#include "core/Icons.h"
#include "core/KnobScale.h"
#include "core/Paint.h"
#include "core/Theme.h"

namespace t3k::ui {

namespace {
constexpr int kImageFadeMs = 200;
constexpr float kDimmedImage = 0.35f;
const juce::Colour kStrip = juce::Colours::black.withAlpha(0.35f);
constexpr int kOutPad = 6;

Knob::Options outKnobOptions() {
  Knob::Options o;
  o.label = "Out";
  o.size = theme::kKnobSizeSecondary;
  o.thumb = Knob::Thumb::secondary;
  o.scale = &scales::gainDb();
  o.defaultValue = 0.5f;
  o.help = help::Key::tileOut;
  o.labelBright = true;
  return o;
}
}  // namespace

void ToneTile::OutPlate::paint(juce::Graphics& g) {
  g.setColour(juce::Colours::black.withAlpha(0.55f));
  g.fillRoundedRectangle(getLocalBounds().toFloat(), 10.0f);
}

void ToneTile::Strip::paint(juce::Graphics& g) {
  g.setColour(kStrip);
  g.fillRect(getLocalBounds().withHeight(kChromeHeight));
}

ToneTile::ToneTile(Services& services, const ChainItem& block, int size)
    : GalleryTile(services, block.blockId, size),
      block_(block),
      image_(services.images),
      power_(Icon::Power, ChromeIconButton::Tone::power, help::Key::blockPower),
      swap_(Icon::ArrowLeftRight, ChromeIconButton::Tone::plain, help::Key::swapTone),
      remove_(Icon::Trash2, ChromeIconButton::Tone::plain, help::Key::removeBlock),
      glow_(services.meters, MeterStore::blockOutId(block.blockId)),
      led_(services.meters, MeterStore::blockOutId(block.blockId)),
      out_(outKnobOptions()) {
  image_.setCornerRadius(gallery::kTileCorner);
  image_.setGlow(glow_.glow());
  glow_.onChange = [this] { image_.setGlow(glow_.glow()); };
  addAndMakeVisible(image_);
  addChildComponent(dots_);
  addChildComponent(retry_);
  retry_.onRetry = [this] { this->services().modelLoads.retry(blockId()); };

  addAndMakeVisible(chrome_);
  for (auto* b : {&power_, &swap_, &remove_}) chrome_.addAndMakeVisible(*b);
  power_.onClick = [this] { togglePower(); };
  // Plugin blocks repurpose the swap slot: open the plugin's own window.
  swap_.onClick = [this] {
    if (block_.tone.plugin)
      this->services().hostedPlugins.openEditor(blockId());
    else if (onSwap)
      onSwap(blockId());
  };
  remove_.onClick = [this] { this->services().chain.removeBlock(blockId()); };

  out_.onChange = [this](float v) {
    this->services().chain.setBlockParam(blockId(), "outputGain", static_cast<double>(v));
  };
  outPlate_.addAndMakeVisible(out_);
  addChildComponent(outPlate_);

  addAndMakeVisible(ledSlot_);
  ledSlot_.addChildComponent(led_);
  ledSlot_.setSize(BlockLed::kSize, BlockLed::kSize);
  addMouseListener(&hover_, true);
  services.pointer.addListener(this);

  setBlock(block);
  setHovered(false);
  resized();  // the base set the size before these children existed
}

ToneTile::~ToneTile() {
  services().pointer.removeListener(this);
  removeMouseListener(&hover_);
}

void ToneTile::setBlock(const ChainItem& block) {
  block_ = block;
  enabled_ = block.params.enabled;
  setHelpText(help::toneTile(block.tone.title));
  setTitle(block.tone.title);
  swap_.setIcon(block.tone.plugin ? Icon::ExternalLink : Icon::ArrowLeftRight);
  swap_.setHelpText(help::text(block.tone.plugin ? help::Key::openPluginEditor : help::Key::swapTone));
  image_.setTone(block.tone.image, block.tone.gear, block.tone.local, kGlyphSize);
  image_.setArtwork(block.tone.plugin ? services().hostedPlugins.iconFor(block.tone.pluginPath, block.tone.title,
                                                                         block.tone.user ? block.tone.user->username
                                                                                         : juce::String())
                                      : juce::Image());
  syncState();
}

// A model download/prepare is in flight: `modelLoading` covers switches
// (the previous model keeps playing, so `loaded` stays true) and `!loaded`
// covers fresh blocks that have nothing to play yet.
void ToneTile::syncState() {
  const bool busy = block_.modelLoading || (!block_.loaded && !block_.loadFailed);
  const bool armed = dropArmed();
  image_.setVisible(!armed);
  imageFade_.animateTo(enabled_ && !busy && !block_.loadFailed ? 1.0f : kDimmedImage, kImageFadeMs);
  dots_.setVisible(!armed && busy && !block_.loadFailed);
  out_.setValue(static_cast<float>(block_.params.outputGain));
  outPlate_.setVisible(!armed && block_.loaded);
  retry_.setVisible(!armed && block_.loadFailed);
  chrome_.setVisible(!armed);
  ledSlot_.setVisible(!armed);
  power_.setOn(enabled_);
  repaint();
}

void ToneTile::togglePower() {
  enabled_ = !enabled_;
  power_.setOn(enabled_);
  imageFade_.animateTo(enabled_ ? 1.0f : kDimmedImage, kImageFadeMs);
  this->services().chain.setBlockParam(blockId(), "enabled", enabled_);
}

void ToneTile::open() {
  if (onOpen) onOpen(blockId());
}

std::vector<ContextMenu::Item> ToneTile::menuItems() {
  std::vector<ContextMenu::Item> items{
      {"Copy", Icon::Copy, help::Key::copyBlock,
       [this] { this->services().chain.copyBlock(blockId()); }},
  };
  if (block_.tone.plugin) {
    items.push_back({"Open Plugin", Icon::ExternalLink, help::Key::openPluginEditor,
                     [this] { this->services().hostedPlugins.openEditor(blockId()); }});
    return items;
  }
  for (auto& item : localLoadItems()) items.push_back(std::move(item));
  return items;
}

void ToneTile::dropArmedChanged(bool) { syncState(); }

void ToneTile::travellingChanged(bool) { setHovered(hovered_); }

void ToneTile::pointerMoved(const juce::MouseEvent& e, bool leaving) {
  // Leaving onto one of our own children is still hovering the tile.
  setHovered(!leaving || getLocalBounds().contains(e.getEventRelativeTo(this).getPosition()));
}

void ToneTile::setHovered(bool hovered) {
  hovered_ = hovered;
  const bool shown = services().pointer.coarse() || hovered_ || travelling();
  chrome_.setAlpha(shown ? 1.0f : 0.0f);
  chrome_.setInterceptsMouseClicks(false, shown);
}

void ToneTile::resized() {
  const auto box = getLocalBounds();
  image_.setBounds(box);
  dots_.setBounds(box.withSizeKeepingCentre(LoadingDots::kWidth, LoadingDots::kDot + 2 * LoadingDots::kMargin));
  retry_.setBounds(box.withSizeKeepingCentre(retry_.getWidth(), retry_.getHeight()));

  chrome_.setBounds(box.withHeight(kChromeHeight));
  const int y = kChromePad;
  power_.setBounds(kChromePad, y, theme::kIconBoxSize, theme::kIconBoxSize);
  remove_.setBounds(getWidth() - kChromePad - theme::kIconBoxSize, y, theme::kIconBoxSize,
                    theme::kIconBoxSize);
  swap_.setBounds(remove_.getX() - kChromeGap - theme::kIconBoxSize, y, theme::kIconBoxSize,
                  theme::kIconBoxSize);

  const int knobH = Knob::heightFor(theme::kKnobSizeSecondary);
  const int plateW = theme::kKnobSizeSecondary + 2 * kOutPad + 8;
  outPlate_.setBounds(kLedInset, getHeight() - kLedInset - knobH - 2 * kOutPad, plateW, knobH + 2 * kOutPad);
  out_.setBounds(outPlate_.getLocalBounds().reduced(kOutPad));

  ledSlot_.setTopLeftPosition(getWidth() - kLedInset - BlockLed::kSize,
                              getHeight() - kLedInset - BlockLed::kSize);
}

void ToneTile::paint(juce::Graphics& g) {
  paint::fill(g, getLocalBounds().toFloat(), gallery::kTileCorner, theme::kSurface);
  if (dropArmed()) {
    const float s = gallery::kFileDropGlyphSize;
    Icons::draw(g, Icon::Upload, getLocalBounds().toFloat().withSizeKeepingCentre(s, s), theme::kGray);
  }
}

bool ToneTile::isInterestedInFileDrag(const juce::StringArray& files) {
  return !block_.tone.plugin && GalleryTile::isInterestedInFileDrag(files);
}

void ToneTile::paintOverChildren(juce::Graphics& g) {
  // Every plugin shares the rack glyph, so the tile names it.
  if (block_.tone.plugin && !dropArmed()) {
    auto label = getLocalBounds().reduced(8).removeFromBottom(36).withTrimmedRight(BlockLed::kSize + 4);
    if (outPlate_.isVisible()) label.setLeft(outPlate_.getRight() + 8);
    g.setColour(theme::kWhite.withAlpha(enabled_ ? 0.9f : 0.45f));
    g.setFont(Fonts::sans(13.0f, true));
    g.drawFittedText(block_.tone.title, label, juce::Justification::bottomLeft, 2, 0.85f);
  }
  if (dropArmed())
    paint::dashedBorder(g, getLocalBounds().toFloat(), gallery::kTileCorner,
                        gallery::kFileDropBorder, gallery::kAddTileBorderWidth);
}

}  // namespace t3k::ui
