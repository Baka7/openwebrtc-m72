// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/media/html_media_element.h"

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/platform/autoplay.mojom-blink.h"
#include "third_party/blink/public/platform/web_media_player_source.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/html/media/html_audio_element.h"
#include "third_party/blink/renderer/core/html/media/html_video_element.h"
#include "third_party/blink/renderer/core/html/media/media_error.h"
#include "third_party/blink/renderer/core/html/track/audio_track_list.h"
#include "third_party/blink/renderer/core/html/track/video_track_list.h"
#include "third_party/blink/renderer/core/loader/empty_clients.h"
#include "third_party/blink/renderer/core/testing/dummy_page_holder.h"
#include "third_party/blink/renderer/platform/network/network_state_notifier.h"
#include "third_party/blink/renderer/platform/testing/empty_web_media_player.h"
#include "third_party/blink/renderer/platform/testing/unit_test_helpers.h"

using ::testing::AnyNumber;
using ::testing::Return;
using ::testing::_;

namespace blink {

class MockWebMediaPlayer : public EmptyWebMediaPlayer {
 public:
  MOCK_CONST_METHOD0(HasAudio, bool());
  MOCK_CONST_METHOD0(HasVideo, bool());
  MOCK_CONST_METHOD0(Duration, double());
  MOCK_CONST_METHOD0(CurrentTime, double());
  MOCK_METHOD1(EnabledAudioTracksChanged, void(const WebVector<TrackId>&));
  MOCK_METHOD1(SelectedVideoTrackChanged, void(TrackId*));
  MOCK_METHOD3(
      Load,
      WebMediaPlayer::LoadTiming(LoadType load_type,
                                 const blink::WebMediaPlayerSource& source,
                                 CorsMode cors_mode));
  MOCK_CONST_METHOD0(DidLazyLoad, bool());
};

class WebMediaStubLocalFrameClient : public EmptyLocalFrameClient {
 public:
  static WebMediaStubLocalFrameClient* Create(
      std::unique_ptr<WebMediaPlayer> player) {
    return new WebMediaStubLocalFrameClient(std::move(player));
  }

  WebMediaStubLocalFrameClient(std::unique_ptr<WebMediaPlayer> player)
      : player_(std::move(player)) {}

  std::unique_ptr<WebMediaPlayer> CreateWebMediaPlayer(
      HTMLMediaElement&,
      const WebMediaPlayerSource&,
      WebMediaPlayerClient* client,
      WebLayerTreeView*) override {
    DCHECK(player_) << " Empty injected player - already used?";
    return std::move(player_);
  }

 private:
  std::unique_ptr<WebMediaPlayer> player_;
};

enum class MediaTestParam { kAudio, kVideo };

class HTMLMediaElementTest : public testing::TestWithParam<MediaTestParam> {
 protected:
  void SetUp() override {
    // Sniff the media player pointer to facilitate mocking.
    auto mock_media_player = std::make_unique<MockWebMediaPlayer>();
    media_player_ = mock_media_player.get();

    // Most tests do not care about this call, nor its return value. Those that
    // do will clear this expectation and set custom expectations/returns.
    EXPECT_CALL(*mock_media_player, HasAudio())
        .WillRepeatedly(testing::Return(true));
    EXPECT_CALL(*mock_media_player, HasVideo())
        .WillRepeatedly(testing::Return(true));
    EXPECT_CALL(*mock_media_player, Duration())
        .WillRepeatedly(testing::Return(1.0));
    EXPECT_CALL(*mock_media_player, CurrentTime())
        .WillRepeatedly(testing::Return(0));
    EXPECT_CALL(*mock_media_player, Load(_, _, _))
        .Times(AnyNumber())
        .WillRepeatedly(Return(WebMediaPlayer::LoadTiming::kImmediate));
    EXPECT_CALL(*mock_media_player, DidLazyLoad)
        .WillRepeatedly(testing::Return(false));

    dummy_page_holder_ = DummyPageHolder::Create(
        IntSize(), nullptr,
        WebMediaStubLocalFrameClient::Create(std::move(mock_media_player)),
        nullptr);

    if (GetParam() == MediaTestParam::kAudio)
      media_ = HTMLAudioElement::Create(dummy_page_holder_->GetDocument());
    else
      media_ = HTMLVideoElement::Create(dummy_page_holder_->GetDocument());
  }

  HTMLMediaElement* Media() const { return media_.Get(); }
  void SetCurrentSrc(const AtomicString& src) {
    KURL url(src);
    Media()->current_src_ = url;
  }

  MockWebMediaPlayer* MockMediaPlayer() { return media_player_; }

  bool WasAutoplayInitiated() { return Media()->WasAutoplayInitiated(); }

  bool CouldPlayIfEnoughData() { return Media()->CouldPlayIfEnoughData(); }

  bool ShouldDelayLoadEvent() { return Media()->should_delay_load_event_; }

  void SetReadyState(HTMLMediaElement::ReadyState state) {
    Media()->SetReadyState(state);
  }

  void SetError(MediaError* err) { Media()->MediaEngineError(err); }

  void SimulateHighMediaEngagement() {
    Media()->GetDocument().GetPage()->AddAutoplayFlags(
        mojom::blink::kAutoplayFlagHighMediaEngagement);
  }

  bool HasLazyLoadObserver() const {
    return !!Media()->lazy_load_visibility_observer_;
  }

 private:
  std::unique_ptr<DummyPageHolder> dummy_page_holder_;
  Persistent<HTMLMediaElement> media_;

  // Owned by WebMediaStubLocalFrameClient.
  MockWebMediaPlayer* media_player_;
};

INSTANTIATE_TEST_CASE_P(Audio,
                        HTMLMediaElementTest,
                        testing::Values(MediaTestParam::kAudio));
INSTANTIATE_TEST_CASE_P(Video,
                        HTMLMediaElementTest,
                        testing::Values(MediaTestParam::kVideo));

TEST_P(HTMLMediaElementTest, effectiveMediaVolume) {
  struct TestData {
    double volume;
    bool muted;
    double effective_volume;
  } test_data[] = {
      {0.0, false, 0.0}, {0.5, false, 0.5}, {1.0, false, 1.0},
      {0.0, true, 0.0},  {0.5, true, 0.0},  {1.0, true, 0.0},
  };

  for (const auto& data : test_data) {
    Media()->setVolume(data.volume);
    Media()->setMuted(data.muted);
    EXPECT_EQ(data.effective_volume, Media()->EffectiveMediaVolume());
  }
}

enum class TestURLScheme {
  kHttp,
  kHttps,
  kFtp,
  kFile,
  kData,
  kBlob,
};

AtomicString SrcSchemeToURL(TestURLScheme scheme) {
  switch (scheme) {
    case TestURLScheme::kHttp:
      return "http://example.com/foo.mp4";
    case TestURLScheme::kHttps:
      return "https://example.com/foo.mp4";
    case TestURLScheme::kFtp:
      return "ftp://example.com/foo.mp4";
    case TestURLScheme::kFile:
      return "file:///foo/bar.mp4";
    case TestURLScheme::kData:
      return "data:video/mp4;base64,XXXXXXX";
    case TestURLScheme::kBlob:
      return "blob:http://example.com/00000000-0000-0000-0000-000000000000";
    default:
      NOTREACHED();
  }
  return g_empty_atom;
}

TEST_P(HTMLMediaElementTest, preloadType) {
  struct TestData {
    bool data_saver_enabled;
    bool force_preload_none_for_media_elements;
    bool is_cellular;
    TestURLScheme src_scheme;
    AtomicString preload_to_set;
    AtomicString preload_expected;
  } test_data[] = {
      // Tests for conditions in which preload type should be overriden to
      // "none".
      {false, true, false, TestURLScheme::kHttp, "auto", "none"},
      {true, true, false, TestURLScheme::kHttps, "auto", "none"},
      {true, true, false, TestURLScheme::kFtp, "metadata", "none"},
      {false, false, false, TestURLScheme::kHttps, "auto", "auto"},
      {false, true, false, TestURLScheme::kFile, "auto", "auto"},
      {false, true, false, TestURLScheme::kData, "metadata", "metadata"},
      {false, true, false, TestURLScheme::kBlob, "auto", "auto"},
      {false, true, false, TestURLScheme::kFile, "none", "none"},
      // Tests for conditions in which preload type should be overriden to
      // "metadata".
      {false, false, true, TestURLScheme::kHttp, "auto", "metadata"},
      {false, false, true, TestURLScheme::kHttp, "scheme", "metadata"},
      {false, false, true, TestURLScheme::kHttp, "none", "none"},
      // Tests that the preload is overriden to "metadata".
      {false, false, false, TestURLScheme::kHttp, "foo", "metadata"},
  };

  int index = 0;
  for (const auto& data : test_data) {
    GetNetworkStateNotifier().SetSaveDataEnabledOverride(
        data.data_saver_enabled);
    Media()->GetDocument().GetSettings()->SetForcePreloadNoneForMediaElements(
        data.force_preload_none_for_media_elements);
    if (data.is_cellular) {
      GetNetworkStateNotifier().SetNetworkConnectionInfoOverride(
          true, WebConnectionType::kWebConnectionTypeCellular3G,
          WebEffectiveConnectionType::kTypeUnknown, 1.0, 2.0);
    } else {
      GetNetworkStateNotifier().ClearOverride();
    }
    SetCurrentSrc(SrcSchemeToURL(data.src_scheme));
    Media()->setPreload(data.preload_to_set);

    EXPECT_EQ(data.preload_expected, Media()->preload())
        << "preload type differs at index" << index;
    ++index;
  }
}

TEST_P(HTMLMediaElementTest, CouldPlayIfEnoughDataRespondsToPlay) {
  EXPECT_FALSE(CouldPlayIfEnoughData());
  Media()->Play();
  EXPECT_TRUE(CouldPlayIfEnoughData());
}

TEST_P(HTMLMediaElementTest, CouldPlayIfEnoughDataRespondsToEnded) {
  Media()->SetSrc(SrcSchemeToURL(TestURLScheme::kHttp));
  Media()->Play();

  test::RunPendingTasks();

  MockWebMediaPlayer* mock_wmpi =
      reinterpret_cast<MockWebMediaPlayer*>(Media()->GetWebMediaPlayer());
  EXPECT_NE(mock_wmpi, nullptr);
  EXPECT_TRUE(CouldPlayIfEnoughData());

  // Playback can only end once the ready state is above kHaveMetadata.
  SetReadyState(HTMLMediaElement::kHaveFutureData);
  EXPECT_FALSE(Media()->paused());
  EXPECT_FALSE(Media()->ended());
  EXPECT_TRUE(CouldPlayIfEnoughData());

  // Now advance current time to duration and verify ended state.
  testing::Mock::VerifyAndClearExpectations(mock_wmpi);
  EXPECT_CALL(*mock_wmpi, CurrentTime())
      .WillRepeatedly(testing::Return(Media()->duration()));
  EXPECT_FALSE(CouldPlayIfEnoughData());
  EXPECT_TRUE(Media()->ended());
}

TEST_P(HTMLMediaElementTest, CouldPlayIfEnoughDataRespondsToError) {
  Media()->SetSrc(SrcSchemeToURL(TestURLScheme::kHttp));
  Media()->Play();

  test::RunPendingTasks();

  MockWebMediaPlayer* mock_wmpi =
      reinterpret_cast<MockWebMediaPlayer*>(Media()->GetWebMediaPlayer());
  EXPECT_NE(mock_wmpi, nullptr);
  EXPECT_TRUE(CouldPlayIfEnoughData());

  SetReadyState(HTMLMediaElement::kHaveMetadata);
  EXPECT_FALSE(Media()->paused());
  EXPECT_FALSE(Media()->ended());
  EXPECT_TRUE(CouldPlayIfEnoughData());

  SetError(MediaError::Create(MediaError::kMediaErrDecode, ""));
  EXPECT_FALSE(CouldPlayIfEnoughData());
}

TEST_P(HTMLMediaElementTest, CouldPlayIfEnoughDataInfiniteStreamNeverEnds) {
  Media()->SetSrc(SrcSchemeToURL(TestURLScheme::kHttp));
  Media()->Play();

  test::RunPendingTasks();

  EXPECT_CALL(*MockMediaPlayer(), Duration())
      .WillRepeatedly(testing::Return(std::numeric_limits<double>::infinity()));
  EXPECT_CALL(*MockMediaPlayer(), CurrentTime())
      .WillRepeatedly(testing::Return(std::numeric_limits<double>::infinity()));

  SetReadyState(HTMLMediaElement::kHaveMetadata);
  EXPECT_FALSE(Media()->paused());
  EXPECT_FALSE(Media()->ended());
  EXPECT_TRUE(CouldPlayIfEnoughData());
}

TEST_P(HTMLMediaElementTest, AutoplayInitiated_DocumentActivation_Low_Gesture) {
  // Setup is the following:
  // - Policy: DocumentUserActivation (aka. unified autoplay)
  // - MEI: low;
  // - Frame received user gesture.
  RuntimeEnabledFeatures::SetMediaEngagementBypassAutoplayPoliciesEnabled(true);
  Media()->GetDocument().GetSettings()->SetAutoplayPolicy(
      AutoplayPolicy::Type::kDocumentUserActivationRequired);
  LocalFrame::NotifyUserActivation(Media()->GetDocument().GetFrame());

  Media()->Play();

  EXPECT_FALSE(WasAutoplayInitiated());
}

TEST_P(HTMLMediaElementTest,
       AutoplayInitiated_DocumentActivation_High_Gesture) {
  // Setup is the following:
  // - Policy: DocumentUserActivation (aka. unified autoplay)
  // - MEI: high;
  // - Frame received user gesture.
  RuntimeEnabledFeatures::SetMediaEngagementBypassAutoplayPoliciesEnabled(true);
  Media()->GetDocument().GetSettings()->SetAutoplayPolicy(
      AutoplayPolicy::Type::kDocumentUserActivationRequired);
  SimulateHighMediaEngagement();
  LocalFrame::NotifyUserActivation(Media()->GetDocument().GetFrame());

  Media()->Play();

  EXPECT_FALSE(WasAutoplayInitiated());
}

TEST_P(HTMLMediaElementTest,
       AutoplayInitiated_DocumentActivation_High_NoGesture) {
  // Setup is the following:
  // - Policy: DocumentUserActivation (aka. unified autoplay)
  // - MEI: high;
  // - Frame did not receive user gesture.
  RuntimeEnabledFeatures::SetMediaEngagementBypassAutoplayPoliciesEnabled(true);
  Media()->GetDocument().GetSettings()->SetAutoplayPolicy(
      AutoplayPolicy::Type::kDocumentUserActivationRequired);
  SimulateHighMediaEngagement();

  Media()->Play();

  EXPECT_TRUE(WasAutoplayInitiated());
}

TEST_P(HTMLMediaElementTest, AutoplayInitiated_GestureRequired_Gesture) {
  // Setup is the following:
  // - Policy: user gesture is required.
  // - Frame received a user gesture.
  // - MEI doesn't matter as it's not used by the policy.
  Media()->GetDocument().GetSettings()->SetAutoplayPolicy(
      AutoplayPolicy::Type::kUserGestureRequired);
  LocalFrame::NotifyUserActivation(Media()->GetDocument().GetFrame());

  Media()->Play();

  EXPECT_FALSE(WasAutoplayInitiated());
}

TEST_P(HTMLMediaElementTest, AutoplayInitiated_NoGestureRequired_Gesture) {
  // Setup is the following:
  // - Policy: no user gesture is required.
  // - Frame received a user gesture.
  // - MEI doesn't matter as it's not used by the policy.
  Media()->GetDocument().GetSettings()->SetAutoplayPolicy(
      AutoplayPolicy::Type::kNoUserGestureRequired);
  LocalFrame::NotifyUserActivation(Media()->GetDocument().GetFrame());

  Media()->Play();

  EXPECT_FALSE(WasAutoplayInitiated());
}

TEST_P(HTMLMediaElementTest, AutoplayInitiated_NoGestureRequired_NoGesture) {
  // Setup is the following:
  // - Policy: no user gesture is required.
  // - Frame did not receive a user gesture.
  // - MEI doesn't matter as it's not used by the policy.
  Media()->GetDocument().GetSettings()->SetAutoplayPolicy(
      AutoplayPolicy::Type::kNoUserGestureRequired);

  Media()->Play();

  EXPECT_TRUE(WasAutoplayInitiated());
}

TEST_P(HTMLMediaElementTest,
       DeferredMediaPlayerLoadDoesNotDelayWindowLoadEvent) {
  // Source isn't really important, we just need something to let load algorithm
  // run up to the point of calling WebMediaPlayer::Load().
  Media()->SetSrc(SrcSchemeToURL(TestURLScheme::kHttp));

  // WebMediaPlayer will signal that it will defer loading to some later time.
  testing::Mock::VerifyAndClearExpectations(MockMediaPlayer());
  EXPECT_CALL(*MockMediaPlayer(), Load(_, _, _))
      .WillOnce(Return(WebMediaPlayer::LoadTiming::kDeferred));

  // Window's 'load' event starts out "delayed".
  EXPECT_TRUE(ShouldDelayLoadEvent());
  Media()->load();
  test::RunPendingTasks();

  // No longer delayed because WMP loading is deferred.
  EXPECT_FALSE(ShouldDelayLoadEvent());
}

TEST_P(HTMLMediaElementTest, ImmediateMediaPlayerLoadDoesDelayWindowLoadEvent) {
  // Source isn't really important, we just need something to let load algorithm
  // run up to the point of calling WebMediaPlayer::Load().
  Media()->SetSrc(SrcSchemeToURL(TestURLScheme::kHttp));

  // WebMediaPlayer will signal that it will do the load immediately.
  EXPECT_CALL(*MockMediaPlayer(), Load(_, _, _))
      .WillOnce(Return(WebMediaPlayer::LoadTiming::kImmediate));

  // Window's 'load' event starts out "delayed".
  EXPECT_TRUE(ShouldDelayLoadEvent());
  Media()->load();
  test::RunPendingTasks();

  // Still delayed because WMP loading is not deferred.
  EXPECT_TRUE(ShouldDelayLoadEvent());
}

TEST_P(HTMLMediaElementTest, DefaultTracksAreEnabled) {
  // Default tracks should start enabled, not be created then enabled.
  EXPECT_CALL(*MockMediaPlayer(), EnabledAudioTracksChanged(_)).Times(0);
  EXPECT_CALL(*MockMediaPlayer(), SelectedVideoTrackChanged(_)).Times(0);

  Media()->SetSrc(SrcSchemeToURL(TestURLScheme::kHttp));
  test::RunPendingTasks();
  SetReadyState(HTMLMediaElement::kHaveFutureData);

  ASSERT_EQ(1u, Media()->audioTracks().length());
  ASSERT_EQ(1u, Media()->videoTracks().length());
  EXPECT_TRUE(Media()->audioTracks().AnonymousIndexedGetter(0)->enabled());
  EXPECT_TRUE(Media()->videoTracks().AnonymousIndexedGetter(0)->selected());
}

// Ensure a visibility observer is created for lazy loading.
TEST_P(HTMLMediaElementTest, VisibilityObserverCreatedForLazyLoad) {
  Media()->SetSrc(SrcSchemeToURL(TestURLScheme::kHttp));
  test::RunPendingTasks();

  EXPECT_CALL(*MockMediaPlayer(), DidLazyLoad()).WillRepeatedly(Return(true));

  SetReadyState(HTMLMediaElement::kHaveFutureData);
  EXPECT_EQ(HasLazyLoadObserver(), GetParam() == MediaTestParam::kVideo);
}

TEST_P(HTMLMediaElementTest, DomInteractive) {
  EXPECT_FALSE(Media()->GetDocument().GetTiming().DomInteractive().is_null());
}

}  // namespace blink
