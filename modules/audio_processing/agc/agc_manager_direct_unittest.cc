/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/audio_processing/agc/agc_manager_direct.h"

#include "modules/audio_processing/agc/gain_control.h"
#include "modules/audio_processing/agc/mock_agc.h"
#include "modules/audio_processing/include/mock_audio_processing.h"
#include "test/field_trial.h"
#include "test/gmock.h"
#include "test/gtest.h"

using ::testing::_;
using ::testing::AtLeast;
using ::testing::DoAll;
using ::testing::Return;
using ::testing::SetArgPointee;

namespace webrtc {
namespace {

const int kSampleRateHz = 32000;
const int kNumChannels = 1;
const int kSamplesPerChannel = kSampleRateHz / 100;
const int kInitialVolume = 128;
constexpr int kClippedMin = 165;  // Arbitrary, but different from the default.
const float kAboveClippedThreshold = 0.2f;
const int kMinMicLevel = 12;

class MockGainControl : public GainControl {
 public:
  virtual ~MockGainControl() {}
  MOCK_METHOD1(Enable, int(bool enable));
  MOCK_CONST_METHOD0(is_enabled, bool());
  MOCK_METHOD1(set_stream_analog_level, int(int level));
  MOCK_CONST_METHOD0(stream_analog_level, int());
  MOCK_METHOD1(set_mode, int(Mode mode));
  MOCK_CONST_METHOD0(mode, Mode());
  MOCK_METHOD1(set_target_level_dbfs, int(int level));
  MOCK_CONST_METHOD0(target_level_dbfs, int());
  MOCK_METHOD1(set_compression_gain_db, int(int gain));
  MOCK_CONST_METHOD0(compression_gain_db, int());
  MOCK_METHOD1(enable_limiter, int(bool enable));
  MOCK_CONST_METHOD0(is_limiter_enabled, bool());
  MOCK_METHOD2(set_analog_level_limits, int(int minimum, int maximum));
  MOCK_CONST_METHOD0(analog_level_minimum, int());
  MOCK_CONST_METHOD0(analog_level_maximum, int());
  MOCK_CONST_METHOD0(stream_is_saturated, bool());
};

class TestVolumeCallbacks : public VolumeCallbacks {
 public:
  TestVolumeCallbacks() : volume_(0) {}
  void SetMicVolume(int volume) override { volume_ = volume; }
  int GetMicVolume() override { return volume_; }

 private:
  int volume_;
};

}  // namespace

class AgcManagerDirectTest : public ::testing::Test {
 protected:
  AgcManagerDirectTest()
      : agc_(new MockAgc),
        manager_(agc_, &gctrl_, &volume_, kInitialVolume, kClippedMin),
        audio(kNumChannels),
        audio_data(kNumChannels * kSamplesPerChannel, 0.f) {
    ExpectInitialize();
    manager_.Initialize();
    for (size_t ch = 0; ch < kNumChannels; ++ch) {
      audio[ch] = &audio_data[ch * kSamplesPerChannel];
    }
  }

  void FirstProcess() {
    EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
    EXPECT_CALL(*agc_, GetRmsErrorDb(_)).WillOnce(Return(false));
    CallProcess(1);
  }

  void SetVolumeAndProcess(int volume) {
    volume_.SetMicVolume(volume);
    FirstProcess();
  }

  void ExpectCheckVolumeAndReset(int volume) {
    volume_.SetMicVolume(volume);
    EXPECT_CALL(*agc_, Reset());
  }

  void ExpectInitialize() {
    EXPECT_CALL(gctrl_, set_mode(GainControl::kFixedDigital));
    EXPECT_CALL(gctrl_, set_target_level_dbfs(2));
    EXPECT_CALL(gctrl_, set_compression_gain_db(7));
    EXPECT_CALL(gctrl_, enable_limiter(true));
  }

  void CallProcess(int num_calls) {
    for (int i = 0; i < num_calls; ++i) {
      EXPECT_CALL(*agc_, Process(_, _, _)).WillOnce(Return());
      manager_.Process(nullptr, kSamplesPerChannel, kSampleRateHz);
    }
  }

  void CallPreProc(int num_calls, float clipped_ratio) {
    RTC_DCHECK_GE(1.f, clipped_ratio);
    const int num_clipped = kSamplesPerChannel * clipped_ratio;
    std::fill(audio_data.begin(), audio_data.end(), 0.f);
    for (size_t ch = 0; ch < kNumChannels; ++ch) {
      for (int k = 0; k < num_clipped; ++k) {
        audio[ch][k] = 32767.f;
      }
    }

    for (int i = 0; i < num_calls; ++i) {
      manager_.AnalyzePreProcess(audio.data(), kNumChannels,
                                 kSamplesPerChannel);
    }
  }

  MockAgc* agc_;
  MockGainControl gctrl_;
  TestVolumeCallbacks volume_;
  AgcManagerDirect manager_;
  std::vector<float*> audio;
  std::vector<float> audio_data;
};

TEST_F(AgcManagerDirectTest, StartupMinVolumeConfigurationIsRespected) {
  FirstProcess();
  EXPECT_EQ(kInitialVolume, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, MicVolumeResponseToRmsError) {
  FirstProcess();

  // Compressor default; no residual error.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(5), Return(true)));
  CallProcess(1);

  // Inside the compressor's window; no change of volume.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(10), Return(true)));
  CallProcess(1);

  // Above the compressor's window; volume should be increased.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)));
  CallProcess(1);
  EXPECT_EQ(130, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(20), Return(true)));
  CallProcess(1);
  EXPECT_EQ(168, volume_.GetMicVolume());

  // Inside the compressor's window; no change of volume.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(5), Return(true)));
  CallProcess(1);
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(0), Return(true)));
  CallProcess(1);

  // Below the compressor's window; volume should be decreased.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-1), Return(true)));
  CallProcess(1);
  EXPECT_EQ(167, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-1), Return(true)));
  CallProcess(1);
  EXPECT_EQ(163, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-9), Return(true)));
  CallProcess(1);
  EXPECT_EQ(129, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, MicVolumeIsLimited) {
  FirstProcess();

  // Maximum upwards change is limited.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(30), Return(true)));
  CallProcess(1);
  EXPECT_EQ(183, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(30), Return(true)));
  CallProcess(1);
  EXPECT_EQ(243, volume_.GetMicVolume());

  // Won't go higher than the maximum.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(30), Return(true)));
  CallProcess(1);
  EXPECT_EQ(255, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-1), Return(true)));
  CallProcess(1);
  EXPECT_EQ(254, volume_.GetMicVolume());

  // Maximum downwards change is limited.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(194, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(137, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(88, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(54, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(33, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(18, volume_.GetMicVolume());

  // Won't go lower than the minimum.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(12, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, CompressorStepsTowardsTarget) {
  FirstProcess();

  // Compressor default; no call to set_compression_gain_db.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(5), Return(true)))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(20);

  // Moves slowly upwards.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(9), Return(true)))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(8)).WillOnce(Return(0));
  CallProcess(1);

  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(9)).WillOnce(Return(0));
  CallProcess(1);

  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(20);

  // Moves slowly downward, then reverses before reaching the original target.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(5), Return(true)))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(8)).WillOnce(Return(0));
  CallProcess(1);

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(9), Return(true)))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(9)).WillOnce(Return(0));
  CallProcess(1);

  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(20);
}

TEST_F(AgcManagerDirectTest, CompressorErrorIsDeemphasized) {
  FirstProcess();

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(10), Return(true)))
      .WillRepeatedly(Return(false));
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(8)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(9)).WillOnce(Return(0));
  CallProcess(1);
  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(20);

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(0), Return(true)))
      .WillRepeatedly(Return(false));
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(8)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(7)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(6)).WillOnce(Return(0));
  CallProcess(1);
  EXPECT_CALL(gctrl_, set_compression_gain_db(_)).Times(0);
  CallProcess(20);
}

TEST_F(AgcManagerDirectTest, CompressorReachesMaximum) {
  FirstProcess();

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(10), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(10), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(10), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(10), Return(true)))
      .WillRepeatedly(Return(false));
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(8)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(9)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(10)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(11)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(12)).WillOnce(Return(0));
  CallProcess(1);
}

TEST_F(AgcManagerDirectTest, CompressorReachesMinimum) {
  FirstProcess();

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(0), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(0), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(0), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(0), Return(true)))
      .WillRepeatedly(Return(false));
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(6)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(5)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(4)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(3)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(2)).WillOnce(Return(0));
  CallProcess(1);
}

TEST_F(AgcManagerDirectTest, NoActionWhileMuted) {
  manager_.SetCaptureMuted(true);
  manager_.Process(nullptr, kSamplesPerChannel, kSampleRateHz);
}

TEST_F(AgcManagerDirectTest, UnmutingChecksVolumeWithoutRaising) {
  FirstProcess();

  manager_.SetCaptureMuted(true);
  manager_.SetCaptureMuted(false);
  ExpectCheckVolumeAndReset(127);
  // SetMicVolume should not be called.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_)).WillOnce(Return(false));
  CallProcess(1);
  EXPECT_EQ(127, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, UnmutingRaisesTooLowVolume) {
  FirstProcess();

  manager_.SetCaptureMuted(true);
  manager_.SetCaptureMuted(false);
  ExpectCheckVolumeAndReset(11);
  EXPECT_CALL(*agc_, GetRmsErrorDb(_)).WillOnce(Return(false));
  CallProcess(1);
  EXPECT_EQ(12, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, ManualLevelChangeResultsInNoSetMicCall) {
  FirstProcess();

  // Change outside of compressor's range, which would normally trigger a call
  // to SetMicVolume.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)));

  // When the analog volume changes, the gain controller is reset.
  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));

  // GetMicVolume returns a value outside of the quantization slack, indicating
  // a manual volume change.
  ASSERT_NE(volume_.GetMicVolume(), 154);
  volume_.SetMicVolume(154);
  CallProcess(1);
  EXPECT_EQ(154, volume_.GetMicVolume());

  // Do the same thing, except downwards now.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-1), Return(true)));
  volume_.SetMicVolume(100);
  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallProcess(1);
  EXPECT_EQ(100, volume_.GetMicVolume());

  // And finally verify the AGC continues working without a manual change.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-1), Return(true)));
  CallProcess(1);
  EXPECT_EQ(99, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, RecoveryAfterManualLevelChangeFromMax) {
  FirstProcess();

  // Force the mic up to max volume. Takes a few steps due to the residual
  // gain limitation.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillRepeatedly(DoAll(SetArgPointee<0>(30), Return(true)));
  CallProcess(1);
  EXPECT_EQ(183, volume_.GetMicVolume());
  CallProcess(1);
  EXPECT_EQ(243, volume_.GetMicVolume());
  CallProcess(1);
  EXPECT_EQ(255, volume_.GetMicVolume());

  // Manual change does not result in SetMicVolume call.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-1), Return(true)));
  volume_.SetMicVolume(50);
  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallProcess(1);
  EXPECT_EQ(50, volume_.GetMicVolume());

  // Continues working as usual afterwards.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(20), Return(true)));
  CallProcess(1);
  EXPECT_EQ(69, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, RecoveryAfterManualLevelChangeBelowMin) {
  FirstProcess();

  // Manual change below min.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-1), Return(true)));
  // Don't set to zero, which will cause AGC to take no action.
  volume_.SetMicVolume(1);
  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallProcess(1);
  EXPECT_EQ(1, volume_.GetMicVolume());

  // Continues working as usual afterwards.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)));
  CallProcess(1);
  EXPECT_EQ(2, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(30), Return(true)));
  CallProcess(1);
  EXPECT_EQ(11, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(20), Return(true)));
  CallProcess(1);
  EXPECT_EQ(18, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, NoClippingHasNoImpact) {
  FirstProcess();

  CallPreProc(100, 0);
  EXPECT_EQ(128, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, ClippingUnderThresholdHasNoImpact) {
  FirstProcess();

  CallPreProc(1, 0.099);
  EXPECT_EQ(128, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, ClippingLowersVolume) {
  SetVolumeAndProcess(255);

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, 0.2);
  EXPECT_EQ(240, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, WaitingPeriodBetweenClippingChecks) {
  SetVolumeAndProcess(255);

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(240, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, Reset()).Times(0);
  CallPreProc(300, kAboveClippedThreshold);
  EXPECT_EQ(240, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(225, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, ClippingLoweringIsLimited) {
  SetVolumeAndProcess(180);

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(kClippedMin, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, Reset()).Times(0);
  CallPreProc(1000, kAboveClippedThreshold);
  EXPECT_EQ(kClippedMin, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, ClippingMaxIsRespectedWhenEqualToLevel) {
  SetVolumeAndProcess(255);

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(240, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillRepeatedly(DoAll(SetArgPointee<0>(30), Return(true)));
  CallProcess(10);
  EXPECT_EQ(240, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, ClippingMaxIsRespectedWhenHigherThanLevel) {
  SetVolumeAndProcess(200);

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(185, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillRepeatedly(DoAll(SetArgPointee<0>(40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(240, volume_.GetMicVolume());
  CallProcess(10);
  EXPECT_EQ(240, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, MaxCompressionIsIncreasedAfterClipping) {
  SetVolumeAndProcess(210);

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(195, volume_.GetMicVolume());

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(11), Return(true)))
      .WillRepeatedly(Return(false));
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(8)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(9)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(10)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(11)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(12)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(13)).WillOnce(Return(0));
  CallProcess(1);

  // Continue clipping until we hit the maximum surplus compression.
  CallPreProc(300, kAboveClippedThreshold);
  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(180, volume_.GetMicVolume());

  CallPreProc(300, kAboveClippedThreshold);
  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(kClippedMin, volume_.GetMicVolume());

  // Current level is now at the minimum, but the maximum allowed level still
  // has more to decrease.
  CallPreProc(300, kAboveClippedThreshold);
  CallPreProc(1, kAboveClippedThreshold);

  CallPreProc(300, kAboveClippedThreshold);
  CallPreProc(1, kAboveClippedThreshold);

  CallPreProc(300, kAboveClippedThreshold);
  CallPreProc(1, kAboveClippedThreshold);

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(16), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(16), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(16), Return(true)))
      .WillOnce(DoAll(SetArgPointee<0>(16), Return(true)))
      .WillRepeatedly(Return(false));
  CallProcess(19);
  EXPECT_CALL(gctrl_, set_compression_gain_db(14)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(15)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(16)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(17)).WillOnce(Return(0));
  CallProcess(20);
  EXPECT_CALL(gctrl_, set_compression_gain_db(18)).WillOnce(Return(0));
  CallProcess(1);
}

TEST_F(AgcManagerDirectTest, UserCanRaiseVolumeAfterClipping) {
  SetVolumeAndProcess(225);

  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(210, volume_.GetMicVolume());

  // High enough error to trigger a volume check.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(14), Return(true)));
  // User changed the volume.
  volume_.SetMicVolume(250);
  EXPECT_CALL(*agc_, Reset()).Times(AtLeast(1));
  CallProcess(1);
  EXPECT_EQ(250, volume_.GetMicVolume());

  // Move down...
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(-10), Return(true)));
  CallProcess(1);
  EXPECT_EQ(210, volume_.GetMicVolume());
  // And back up to the new max established by the user.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(40), Return(true)));
  CallProcess(1);
  EXPECT_EQ(250, volume_.GetMicVolume());
  // Will not move above new maximum.
  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillOnce(DoAll(SetArgPointee<0>(30), Return(true)));
  CallProcess(1);
  EXPECT_EQ(250, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, ClippingDoesNotPullLowVolumeBackUp) {
  SetVolumeAndProcess(80);

  EXPECT_CALL(*agc_, Reset()).Times(0);
  int initial_volume = volume_.GetMicVolume();
  CallPreProc(1, kAboveClippedThreshold);
  EXPECT_EQ(initial_volume, volume_.GetMicVolume());
}

TEST_F(AgcManagerDirectTest, TakesNoActionOnZeroMicVolume) {
  FirstProcess();

  EXPECT_CALL(*agc_, GetRmsErrorDb(_))
      .WillRepeatedly(DoAll(SetArgPointee<0>(30), Return(true)));
  volume_.SetMicVolume(0);
  CallProcess(10);
  EXPECT_EQ(0, volume_.GetMicVolume());
}

TEST(AgcManagerDirectStandaloneTest, DisableDigitalDisablesDigital) {
  auto agc = std::unique_ptr<Agc>(new ::testing::NiceMock<MockAgc>());
  MockGainControl gctrl;
  TestVolumeCallbacks volume;

  AgcManagerDirect manager(agc.release(), &gctrl, &volume, kInitialVolume,
                           kClippedMin,
                           /* use agc2 level estimation */ false,
                           /* disable digital adaptive */ true);

  EXPECT_CALL(gctrl, set_mode(GainControl::kFixedDigital));
  EXPECT_CALL(gctrl, set_target_level_dbfs(0));
  EXPECT_CALL(gctrl, set_compression_gain_db(0));
  EXPECT_CALL(gctrl, enable_limiter(false));

  manager.Initialize();
}

TEST(AgcManagerDirectStandaloneTest, AgcMinMicLevelExperiment) {
  auto agc_man = std::unique_ptr<AgcManagerDirect>(new AgcManagerDirect(
      nullptr, nullptr, nullptr, kInitialVolume, kClippedMin, true, true));
  EXPECT_EQ(agc_man->min_mic_level(), kMinMicLevel);
  EXPECT_EQ(agc_man->startup_min_level(), kInitialVolume);
  {
    test::ScopedFieldTrials field_trial(
        "WebRTC-Audio-AgcMinMicLevelExperiment/Disabled/");
    agc_man.reset(new AgcManagerDirect(
        nullptr, nullptr, nullptr, kInitialVolume, kClippedMin, true, true));
    EXPECT_EQ(agc_man->min_mic_level(), kMinMicLevel);
    EXPECT_EQ(agc_man->startup_min_level(), kInitialVolume);
  }
  {
    // Valid range of field-trial parameter is [0,255].
    test::ScopedFieldTrials field_trial(
        "WebRTC-Audio-AgcMinMicLevelExperiment/Enabled-256/");
    agc_man.reset(new AgcManagerDirect(
        nullptr, nullptr, nullptr, kInitialVolume, kClippedMin, true, true));
    EXPECT_EQ(agc_man->min_mic_level(), kMinMicLevel);
    EXPECT_EQ(agc_man->startup_min_level(), kInitialVolume);
  }
  {
    test::ScopedFieldTrials field_trial(
        "WebRTC-Audio-AgcMinMicLevelExperiment/Enabled--1/");
    agc_man.reset(new AgcManagerDirect(
        nullptr, nullptr, nullptr, kInitialVolume, kClippedMin, true, true));
    EXPECT_EQ(agc_man->min_mic_level(), kMinMicLevel);
    EXPECT_EQ(agc_man->startup_min_level(), kInitialVolume);
  }
  {
    // Verify that a valid experiment changes the minimum microphone level.
    // The start volume is larger than the min level and should therefore not
    // be changed.
    test::ScopedFieldTrials field_trial(
        "WebRTC-Audio-AgcMinMicLevelExperiment/Enabled-50/");
    agc_man.reset(new AgcManagerDirect(
        nullptr, nullptr, nullptr, kInitialVolume, kClippedMin, true, true));
    EXPECT_EQ(agc_man->min_mic_level(), 50);
    EXPECT_EQ(agc_man->startup_min_level(), kInitialVolume);
  }
  {
    // Use experiment to reduce the default minimum microphone level, start at
    // a lower level and ensure that the startup level is increased to the min
    // level set by the experiment.
    test::ScopedFieldTrials field_trial(
        "WebRTC-Audio-AgcMinMicLevelExperiment/Enabled-50/");
    agc_man.reset(new AgcManagerDirect(nullptr, nullptr, nullptr, 30,
                                       kClippedMin, true, true));
    EXPECT_EQ(agc_man->min_mic_level(), 50);
    EXPECT_EQ(agc_man->startup_min_level(), 50);
  }
}

}  // namespace webrtc
