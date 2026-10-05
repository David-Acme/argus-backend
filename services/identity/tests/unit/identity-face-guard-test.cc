#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/enrollment/services/enrollment-feature-service.hxx>
#include <shared/services/face/anti-spoof.hxx>
#include <shared/services/face/face-check.hxx>
#include <shared/services/face/face-image.hxx>
#include <shared/services/face/inference-slots.hxx>
#include <shared/services/face/member-match.hxx>
#include <shared/services/object-deletion/object-deletion-worker.hxx>

#include <array>
#include <cmath>
#include <coroutine>
#include <limits>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <vector>

namespace
{
std::string encoded(const cv::Mat& image, const std::string& extension)
{
  std::vector<uchar> buffer;
  REQUIRE(cv::imencode(extension, image, buffer));
  return {buffer.begin(), buffer.end()};
}

std::string withExifOrientation(const std::string& jpeg, unsigned char orientation)
{
  const std::array<unsigned char, 36> app1{
      0xFF, 0xE1, 0x00, 0x22, 'E',  'x',  'i',  'f',  0x00, 0x00, 'I',  'I',
      0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x12, 0x01, 0x03, 0x00,
      0x01, 0x00, 0x00, 0x00, orientation, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00};
  std::string out = jpeg.substr(0, 2);
  out.append(reinterpret_cast<const char*>(app1.data()), app1.size());
  out.append(jpeg.substr(2));
  return out;
}

FaceQuality goodFace()
{
  return FaceQuality{.detectorScore = 0.99F,
                     .faceWidthPx = 160.0F,
                     .interOcularPx = 64.0F,
                     .yaw = 0.05F,
                     .pitch = 0.5F,
                     .sharpness = 120.0F};
}
}

TEST_CASE("the anti-spoofing crop follows the upstream scaled box")
{
  const auto centred = anti_spoof::cropBox({.imageWidth = 640,
                                            .imageHeight = 480,
                                            .x = 280.0F,
                                            .y = 200.0F,
                                            .width = 80.0F,
                                            .height = 80.0F,
                                            .scale = 2.7F});
  REQUIRE(centred.has_value());
  CHECK(centred->x1 == 212);
  CHECK(centred->y1 == 132);
  CHECK(centred->x2 == 428);
  CHECK(centred->y2 == 348);

  const auto shifted = anti_spoof::cropBox({.imageWidth = 640,
                                            .imageHeight = 480,
                                            .x = 0.0F,
                                            .y = 0.0F,
                                            .width = 100.0F,
                                            .height = 100.0F,
                                            .scale = 4.0F});
  REQUIRE(shifted.has_value());
  CHECK(shifted->x1 == 0);
  CHECK(shifted->y1 == 0);
  CHECK(shifted->width() == 401);
  CHECK(shifted->height() == 401);

  const auto clamped = anti_spoof::cropBox({.imageWidth = 200,
                                            .imageHeight = 200,
                                            .x = 50.0F,
                                            .y = 50.0F,
                                            .width = 100.0F,
                                            .height = 100.0F,
                                            .scale = 4.0F});
  REQUIRE(clamped.has_value());
  CHECK(clamped->x1 == 0);
  CHECK(clamped->x2 == 199);
  CHECK(clamped->y2 == 199);

  CHECK_FALSE(anti_spoof::cropBox({.imageWidth = 1,
                                   .imageHeight = 1,
                                   .x = 0.0F,
                                   .y = 0.0F,
                                   .width = 1.0F,
                                   .height = 1.0F,
                                   .scale = 2.7F})
                  .has_value());
}

TEST_CASE("the liveness decision is a threshold on the real-face probability")
{
  const std::array<float, kAntiSpoofClasses> logits{0.0F, 2.0F, 0.0F};
  const auto probabilities = anti_spoof::softmax(logits);
  CHECK(probabilities[0] + probabilities[1] + probabilities[2] ==
        doctest::Approx(1.0F));
  CHECK(probabilities[kAntiSpoofRealClass] > 0.78F);

  CHECK(anti_spoof::isLive(0.80F, 0.80F));
  CHECK_FALSE(anti_spoof::isLive(0.79F, 0.80F));
  CHECK_FALSE(anti_spoof::isLive(std::numeric_limits<float>::quiet_NaN(), 0.5F));

  const FaceCheckPolicy required{.quality = face_check::biometricGate(),
                                 .livenessRequired = true,
                                 .livenessThreshold = 0.80F,
                                 .encodePortrait = false};
  CHECK(face_check::livenessVerdict(required, 0.95F) == FaceCheckStatus::Accepted);
  CHECK(face_check::livenessVerdict(required, 0.02F) == FaceCheckStatus::SpoofSuspected);
  FaceCheckPolicy relaxed = required;
  relaxed.livenessRequired = false;
  CHECK(face_check::livenessVerdict(relaxed, 0.02F) == FaceCheckStatus::Accepted);
}

TEST_CASE("liveness fails closed when the check is required and the models are absent")
{
  CHECK_FALSE(face_check::livenessRunnable({.required = true, .engineLoaded = false}));
  CHECK(face_check::livenessRunnable({.required = true, .engineLoaded = true}));
  CHECK(face_check::livenessRunnable({.required = false, .engineLoaded = false}));

  AntiSpoofEngine engine;
  CHECK(engine.load("/nonexistent/anti-spoof") == AntiSpoofLoad::Missing);
  CHECK_FALSE(engine.isLoaded());
  const std::vector<std::uint8_t> pixels(static_cast<std::size_t>(64) * 64 * 3, 128);
  CHECK_FALSE(engine
                  .realScore({.rgbData = pixels.data(),
                              .width = 64,
                              .height = 64,
                              .x1 = 16.0F,
                              .y1 = 16.0F,
                              .x2 = 48.0F,
                              .y2 = 48.0F})
                  .has_value());

  CHECK(EnrollmentFeatureService::outcomeOf(FaceCheckStatus::LivenessUnavailable) ==
        EnrollmentOutcome::LivenessUnavailable);
  CHECK(EnrollmentFeatureService::outcomeOf(FaceCheckStatus::SpoofSuspected) ==
        EnrollmentOutcome::LivenessFailed);
  CHECK(EnrollmentFeatureService::outcomeOf(FaceCheckStatus::MultipleFaces) ==
        EnrollmentOutcome::FaceQualityInsufficient);
  CHECK(EnrollmentFeatureService::outcomeOf(FaceCheckStatus::PoorQuality) ==
        EnrollmentOutcome::FaceQualityInsufficient);
  CHECK(EnrollmentFeatureService::outcomeOf(FaceCheckStatus::NoFace) ==
        EnrollmentOutcome::FaceExtractionFailed);
}

TEST_CASE("login and enrollment take exactly one close, frontal, sharp face")
{
  const FaceQualityGate gate = face_check::biometricGate();
  const FaceQuality good = goodFace();
  CHECK(face_check::screen({.faces = 1, .quality = good, .gate = gate}) ==
        FaceCheckStatus::Accepted);
  CHECK(face_check::screen({.faces = 0, .quality = good, .gate = gate}) ==
        FaceCheckStatus::NoFace);
  CHECK(face_check::screen({.faces = 2, .quality = good, .gate = gate}) ==
        FaceCheckStatus::MultipleFaces);

  FaceQuality small = good;
  small.interOcularPx = face_check::kMinBiometricInterOcularPx - 1.0F;
  CHECK(face_check::screen({.faces = 1, .quality = small, .gate = gate}) ==
        FaceCheckStatus::PoorQuality);
  FaceQuality turned = good;
  turned.yaw = face_check::kMaxBiometricYaw + 0.01F;
  CHECK(face_check::screen({.faces = 1, .quality = turned, .gate = gate}) ==
        FaceCheckStatus::PoorQuality);
  FaceQuality blurred = good;
  blurred.sharpness = 1.0F;
  CHECK(face_check::screen({.faces = 1, .quality = blurred, .gate = gate}) ==
        FaceCheckStatus::PoorQuality);
  CHECK(face_check::statusToString(FaceCheckStatus::SpoofSuspected) ==
        "liveness_failed");
}

TEST_CASE("only JPEG and PNG within 16 MP and 4096 px per side are decoded")
{
  CHECK(face_image::sniff("\xFF\xD8\xFF\xE0") == FaceImageFormat::Jpeg);
  CHECK(face_image::sniff(std::string("\x89PNG\r\n\x1A\n", 8)) == FaceImageFormat::Png);
  CHECK(face_image::sniff("BM\x36\x00") == FaceImageFormat::Unsupported);
  CHECK(face_image::sniff("8BPS") == FaceImageFormat::Unsupported);
  CHECK(face_image::sniff("") == FaceImageFormat::Unsupported);

  CHECK(face_image::acceptsDimensions({.width = 4096, .height = 3000}));
  CHECK_FALSE(face_image::acceptsDimensions({.width = 4097, .height = 10}));
  CHECK_FALSE(face_image::acceptsDimensions({.width = 4096, .height = 4096}));
  CHECK_FALSE(face_image::acceptsDimensions({.width = 0, .height = 10}));

  const cv::Mat wide(10, 4100, CV_8UC3, cv::Scalar(10, 20, 30));
  CHECK(face_image::decodeToRgb(encoded(wide, ".png")).rgb.empty());
  const cv::Mat small(20, 40, CV_8UC3, cv::Scalar(10, 20, 30));
  CHECK(face_image::decodeToRgb(encoded(small, ".bmp")).rgb.empty());
  const auto png = face_image::decodeToRgb(encoded(small, ".png"));
  CHECK(png.width == 40);
  CHECK(png.height == 20);
  REQUIRE(png.rgb.size() == static_cast<std::size_t>(40) * 20 * 3);
  CHECK(png.rgb[0] == 30);
  CHECK(png.rgb[2] == 10);
}

TEST_CASE("EXIF orientation is applied to a decoded JPEG")
{
  const cv::Mat landscape(20, 40, CV_8UC3, cv::Scalar(90, 90, 90));
  const std::string jpeg = encoded(landscape, ".jpg");
  const auto upright = face_image::decodeToRgb(jpeg);
  CHECK(upright.width == 40);
  CHECK(upright.height == 20);
  const auto rotated = face_image::decodeToRgb(withExifOrientation(jpeg, 6));
  CHECK(rotated.width == 20);
  CHECK(rotated.height == 40);
}

TEST_CASE("a stored portrait is a re-encoded face crop without metadata")
{
  const cv::Mat photo(600, 800, CV_8UC3, cv::Scalar(40, 80, 120));
  const auto decoded = face_image::decodeToRgb(encoded(photo, ".jpg"));
  REQUIRE_FALSE(decoded.rgb.empty());
  const std::string crop = face_image::encodeFaceCrop(
      {.rgbData = decoded.rgb.data(),
       .width = decoded.width,
       .height = decoded.height,
       .x1 = 300.0F,
       .y1 = 200.0F,
       .x2 = 500.0F,
       .y2 = 420.0F,
       .margin = face_image::kPortraitMargin,
       .maxSide = face_image::kPortraitMaxSide,
       .quality = face_image::kPortraitQuality});
  REQUIRE(crop.size() > 4);
  CHECK(face_image::sniff(crop) == FaceImageFormat::Jpeg);
  CHECK(crop.find("Exif") == std::string::npos);
  const auto reread = face_image::decodeToRgb(crop);
  CHECK(std::max(reread.width, reread.height) <= face_image::kPortraitMaxSide);
}

TEST_CASE("sign-in matches household members only, past the threshold and the margin")
{
  const std::vector<MemberCandidate> visitorCloser{
      {.personId = 9, .score = 0.92F, .member = false},
      {.personId = 1, .score = 0.71F, .member = true}};
  const auto member = member_match::decide(
      {.candidates = visitorCloser, .threshold = 0.50F, .margin = 0.05F});
  REQUIRE(member.has_value());
  CHECK(member->personId == 1);

  const std::vector<MemberCandidate> twins{
      {.personId = 1, .score = 0.71F, .member = true},
      {.personId = 2, .score = 0.69F, .member = true}};
  CHECK_FALSE(member_match::decide({.candidates = twins, .threshold = 0.50F, .margin = 0.05F})
                  .has_value());
  CHECK(member_match::decide({.candidates = twins, .threshold = 0.50F, .margin = 0.0F})
            .has_value());

  const std::vector<MemberCandidate> weak{
      {.personId = 1, .score = 0.42F, .member = true}};
  CHECK_FALSE(member_match::decide({.candidates = weak, .threshold = 0.50F, .margin = 0.05F})
                  .has_value());
  const std::vector<MemberCandidate> visitorsOnly{
      {.personId = 9, .score = 0.99F, .member = false}};
  CHECK_FALSE(
      member_match::decide({.candidates = visitorsOnly, .threshold = 0.50F, .margin = 0.05F})
          .has_value());
}

TEST_CASE("an inference slot is handed to a waiting coroutine without blocking a thread")
{
  std::vector<std::coroutine_handle<>> resumed;
  InferenceSlots slots([&resumed](std::coroutine_handle<> handle) {
    resumed.push_back(handle);
  });
  CHECK_FALSE(slots.tryAcquire());
  slots.open(1);
  CHECK(slots.available() == 1);

  auto first = slots.acquireAsync();
  CHECK(first.await_ready());
  CHECK(slots.available() == 0);

  auto second = slots.acquireAsync();
  CHECK_FALSE(second.await_ready());
  CHECK(second.await_suspend(std::noop_coroutine()));
  CHECK(slots.waiting() == 1);

  slots.release();
  REQUIRE(resumed.size() == 1);
  CHECK(slots.available() == 0);
  CHECK(slots.waiting() == 0);

  {
    const InferenceSlots::Permit permit(slots);
  }
  CHECK(slots.available() == 1);
  slots.acquire();
  CHECK(slots.available() == 0);
}

TEST_CASE("a private object waiting for storage is retried with a bounded backoff")
{
  constexpr int64_t kNow = 1'000'000;
  CHECK(object_deletion::nextAttemptAt({.attempts = 0, .now = kNow}) ==
        kNow + object_deletion::kBaseBackoffSeconds);
  CHECK(object_deletion::nextAttemptAt({.attempts = 3, .now = kNow}) ==
        kNow + object_deletion::kBaseBackoffSeconds * 8);
  CHECK(object_deletion::nextAttemptAt({.attempts = 50, .now = kNow}) ==
        kNow + object_deletion::kMaxBackoffSeconds);
}
