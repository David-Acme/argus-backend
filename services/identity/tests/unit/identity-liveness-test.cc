#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/face/face-service.hxx>
#include <sqlite/vec-db.hxx>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <vector>

#ifndef ARGUS_TEST_FACE_MODELS
#error "ARGUS_TEST_FACE_MODELS must point at models/face"
#endif
#ifndef ARGUS_TEST_FACE_FIXTURES
#error "ARGUS_TEST_FACE_FIXTURES must point at tests/fixtures/face"
#endif

namespace
{
constexpr const char* kDb = "identity-liveness-test.db";
const std::string kModels = ARGUS_TEST_FACE_MODELS;
const std::string kAntiSpoof = kModels + "/anti-spoof";

bool modelsPresent()
{
  return std::filesystem::exists(kModels + "/detector.bin") &&
         std::filesystem::exists(kModels + "/recognizer.bin") &&
         std::filesystem::exists(kAntiSpoof + "/MiniFASNetV2.onnx") &&
         std::filesystem::exists(kAntiSpoof + "/MiniFASNetV1SE.onnx");
}

std::string fixture(const std::string& name)
{
  std::ifstream in(std::string(ARGUS_TEST_FACE_FIXTURES) + "/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string heldUpPrint(const std::string& name)
{
  const std::string bytes = fixture(name);
  const cv::Mat photo = cv::imdecode(
      cv::Mat(1, static_cast<int>(bytes.size()), CV_8UC1, const_cast<char*>(bytes.data())),
      cv::IMREAD_COLOR);
  REQUIRE_FALSE(photo.empty());
  cv::Mat framed;
  cv::copyMakeBorder(photo, framed, 12, 12, 12, 12, cv::BORDER_CONSTANT,
                     cv::Scalar(240, 240, 240));
  cv::Mat scene(photo.rows * 3, photo.cols * 3, CV_8UC3, cv::Scalar(90, 90, 90));
  framed.copyTo(scene(cv::Rect((scene.cols - framed.cols) / 2,
                               (scene.rows - framed.rows) / 2, framed.cols,
                               framed.rows)));
  std::vector<uchar> buffer;
  REQUIRE(cv::imencode(".jpg", scene, buffer));
  return {buffer.begin(), buffer.end()};
}

FaceCheckPolicy livenessOnly()
{
  return FaceCheckPolicy{.quality = {.minDetectorScore = 0.0F,
                                     .minInterOcularPx = 0.0F,
                                     .maxYaw = 10.0F,
                                     .minPitch = -10.0F,
                                     .maxPitch = 10.0F,
                                     .minSharpness = 0.0F},
                         .livenessRequired = true,
                         .livenessThreshold = 0.80F,
                         .encodePortrait = false};
}

void useTestDatabase()
{
  static const bool prepared = [] {
    for (const char* suffix : {"", "-wal", "-shm"})
      std::remove((std::string(kDb) + suffix).c_str());
    VecDb::instance().setDbFile(kDb);
    return true;
  }();
  CHECK(prepared);
}
}

TEST_CASE("without the anti-spoofing models face sign-in fails closed" *
          doctest::skip(!modelsPresent()))
{
  useTestDatabase();
  FaceService faces;
  faces.init(kModels);
  REQUIRE(faces.isLoaded());
  CHECK(faces.initLiveness(kModels + "/missing") == AntiSpoofLoad::Missing);
  CHECK_FALSE(faces.livenessLoaded());
  const auto check = faces.verifyImage(
      {.imageBytes = fixture("barratt-a.jpg"), .policy = livenessOnly()});
  CHECK(check.status == FaceCheckStatus::LivenessUnavailable);
  CHECK(check.embedding.empty());
}

TEST_CASE("live portraits pass and a photo held up in a frame is refused" *
          doctest::skip(!modelsPresent()))
{
  useTestDatabase();
  auto& faces = FaceService::instance();
  faces.init(kModels);
  REQUIRE(faces.isLoaded());
  REQUIRE(faces.initLiveness(kAntiSpoof) == AntiSpoofLoad::Loaded);

  for (const char* name :
       {"barratt-a.jpg", "barratt-b.jpg", "meir-a.jpg", "hathaway.jpg", "menon.jpg"}) {
    CAPTURE(name);
    const auto live = faces.verifyImage({.imageBytes = fixture(name), .policy = livenessOnly()});
    CHECK(live.status == FaceCheckStatus::Accepted);
    REQUIRE(live.liveness.has_value());
    CHECK(*live.liveness >= 0.80F);
    CHECK(live.embedding.size() == 128);
    MESSAGE(name << " liveness " << *live.liveness << " inter-ocular "
                 << live.quality.interOcularPx << " yaw " << live.quality.yaw
                 << " pitch " << live.quality.pitch << " sharpness "
                 << live.quality.sharpness);

    const auto print =
        faces.verifyImage({.imageBytes = heldUpPrint(name), .policy = livenessOnly()});
    CHECK(print.status == FaceCheckStatus::SpoofSuspected);
    CHECK(print.embedding.empty());
    if (print.liveness)
      MESSAGE(name << " held up in a frame: liveness " << *print.liveness);
  }

  FaceCheckPolicy withPortrait = livenessOnly();
  withPortrait.encodePortrait = true;
  const auto enrolled =
      faces.verifyImage({.imageBytes = fixture("menon.jpg"), .policy = withPortrait});
  REQUIRE(enrolled.status == FaceCheckStatus::Accepted);
  REQUIRE(enrolled.portraitJpeg.size() > 2);
  CHECK(face_image::sniff(enrolled.portraitJpeg) == FaceImageFormat::Jpeg);
}
