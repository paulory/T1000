// Pitch fine tune (T1000's pitchFine, cents on top of the semitone shift).
// It must move the pitch even with STEP on (STEP only quantises the
// semitone knob), so +100 cents at 0 semitones lands a whole semitone up.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

TEST(PitchFineTest, HundredCentsIsOneSemitoneEvenWithStep) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  auto set = [&](const char* id, float normalised) {
    proc.parameters.getParameter(id)->setValueNotifyingHost(normalised);
  };
  set("pitchEnabled", 1.0f);
  set("pitchStep", 1.0f);
  set("pitchSemitones", 0.5f);  // 0 st
  set("pitchFine", 1.0f);       // +100 ct

  const double original = 1000.0;
  const double expected = original * std::pow(2.0, 1.0 / 12.0);
  const auto out = processStereo(proc, makeSine(4 * 48000, original, 0.25f)).first;
  const size_t settle = 2 * 48000;
  const double atExpected = db(goertzelPower(out.data() + settle, 32768, expected));
  const double atOriginal = db(goertzelPower(out.data() + settle, 32768, original));
  EXPECT_GT(atExpected - atOriginal, 20.0) << "expected " << atExpected << " dB, original " << atOriginal;
}
