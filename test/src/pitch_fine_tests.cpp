// Pitch tune (T1000's pitchTuneHz: the A4 reference in Hz on top of the
// semitone shift). It must move the pitch even with STEP on (STEP only
// quantises the semitone knob): the top of the range (466.2 Hz) at 0
// semitones lands a whole semitone up, and 432 Hz lands 432/440 down.
#include "Processor.h"
#include "chain_test_helpers.h"

#include <gtest/gtest.h>

TEST(PitchFineTest, TopOfTheRangeIsOneSemitoneEvenWithStep) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  auto set = [&](const char* id, float normalised) {
    proc.parameters.getParameter(id)->setValueNotifyingHost(normalised);
  };
  set("pitchEnabled", 1.0f);
  set("pitchStep", 1.0f);
  set("pitchSemitones", 0.5f);  // 0 st
  set("pitchTuneHz", 1.0f);     // A4 = 466.2 Hz

  const double original = 1000.0;
  const double expected = original * std::pow(2.0, 1.0 / 12.0);
  const auto out = processStereo(proc, makeSine(4 * 48000, original, 0.25f)).first;
  const size_t settle = 2 * 48000;
  const double atExpected = db(goertzelPower(out.data() + settle, 32768, expected));
  const double atOriginal = db(goertzelPower(out.data() + settle, 32768, original));
  EXPECT_GT(atExpected - atOriginal, 20.0) << "expected " << atExpected << " dB, original " << atOriginal;
}

TEST(PitchFineTest, A432TunesDownByTheRatio) {
  ChainTestProcessor proc;
  proc.setPlayConfigDetails(2, 2, kFs, 512);
  proc.prepareToPlay(kFs, 512);
  proc.parameters.getParameter("pitchEnabled")->setValueNotifyingHost(1.0f);
  auto* tune = proc.parameters.getParameter("pitchTuneHz");
  tune->setValueNotifyingHost(tune->convertTo0to1(432.0f));

  const double original = 1000.0;
  const double expected = original * 432.0 / 440.0;
  const auto out = processStereo(proc, makeSine(4 * 48000, original, 0.25f)).first;
  const size_t settle = 2 * 48000;
  const double atExpected = db(goertzelPower(out.data() + settle, 65536 / 2, expected));
  const double atOriginal = db(goertzelPower(out.data() + settle, 65536 / 2, original));
  EXPECT_GT(atExpected - atOriginal, 15.0) << "expected " << atExpected << " dB, original " << atOriginal;
}
