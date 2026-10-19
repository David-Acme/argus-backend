#include <doctest/doctest.h>

#include <shared/services/privacy/camera-audio-policy.hxx>
#include <shared/services/privacy/privacy-mask.hxx>

#include <cstdint>
#include <opencv2/imgcodecs.hpp>
#include <vector>

namespace
{
argus::identity::v1::ListPrivacyResponse household(bool cameraAudio)
{
  argus::identity::v1::ListPrivacyResponse response;
  response.mutable_household()->set_camera_audio(cameraAudio);
  return response;
}

void addUser(argus::identity::v1::ListPrivacyResponse& response, bool decided,
             bool cameraAudio)
{
  auto* user = response.add_users();
  user->set_user_id(response.users_size());
  user->mutable_choices()->set_decided(decided);
  user->mutable_choices()->set_camera_audio(cameraAudio);
}

std::vector<uint8_t> whiteJpeg()
{
  const cv::Mat white(40, 80, CV_8UC3, cv::Scalar(255, 255, 255));
  std::vector<uint8_t> bytes;
  REQUIRE(cv::imencode(".jpg", white, bytes));
  return bytes;
}

std::vector<OperatorZone> leftHalf()
{
  return {{.cameraId = 6,
           .name = "Vereda",
           .kind = ZoneType::Privacy,
           .points = {{0.0, 0.0}, {0.5, 0.0}, {0.5, 1.0}, {0.0, 1.0}}}};
}
}

TEST_CASE("camera audio needs the household switch and every person's yes")
{
  auto everyone = household(true);
  addUser(everyone, true, true);
  addUser(everyone, true, true);
  CHECK(CameraAudioPolicy::allowedBy(everyone));

  auto oneSaysNo = household(true);
  addUser(oneSaysNo, true, true);
  addUser(oneSaysNo, true, false);
  CHECK_FALSE(CameraAudioPolicy::allowedBy(oneSaysNo));

  auto undecided = household(true);
  addUser(undecided, true, true);
  addUser(undecided, false, false);
  CHECK_FALSE(CameraAudioPolicy::allowedBy(undecided));

  auto switchedOff = household(false);
  addUser(switchedOff, true, true);
  CHECK_FALSE(CameraAudioPolicy::allowedBy(switchedOff));

  CHECK_FALSE(CameraAudioPolicy::allowedBy(household(true)));
}

TEST_CASE("the policy starts withheld and tells its listeners only on a change")
{
  auto& policy = CameraAudioPolicy::instance();
  CHECK_FALSE(policy.allowed());
  std::vector<bool> heard;
  policy.onChange([&heard](bool allowed) { heard.push_back(allowed); });

  auto everyone = household(true);
  addUser(everyone, true, true);
  CHECK(policy.apply(everyone));
  CHECK(policy.allowed());
  CHECK_FALSE(policy.apply(everyone));

  auto withdrawn = household(true);
  addUser(withdrawn, true, false);
  CHECK(policy.apply(withdrawn));
  CHECK_FALSE(policy.allowed());

  CHECK(heard == std::vector<bool>{true, false});
}

TEST_CASE("a privacy mask blanks its polygon and leaves the rest of the frame")
{
  cv::Mat rgb(100, 200, CV_8UC3, cv::Scalar(120, 130, 140));
  const std::vector<OperatorZone> zones{
      {.cameraId = 6,
       .name = "Vereda",
       .kind = ZoneType::Privacy,
       .points = {{0.0, 0.0}, {0.5, 0.0}, {0.5, 1.0}, {0.0, 1.0}}},
      {.cameraId = 6,
       .name = "Puerta",
       .kind = ZoneType::Alert,
       .points = {{0.6, 0.0}, {1.0, 0.0}, {1.0, 1.0}}}};
  REQUIRE(privacy_mask::covers(zones));
  REQUIRE(privacy_mask::apply(rgb, zones));
  CHECK(rgb.at<cv::Vec3b>(50, 40) == cv::Vec3b(0, 0, 0));
  CHECK(rgb.at<cv::Vec3b>(50, 160) == cv::Vec3b(120, 130, 140));
  CHECK(rgb.at<cv::Vec3b>(10, 190) == cv::Vec3b(120, 130, 140));

  const std::vector<OperatorZone> none{{.cameraId = 6,
                                        .name = "Patio",
                                        .kind = ZoneType::Monitor,
                                        .points = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}}}};
  CHECK_FALSE(privacy_mask::covers(none));
  cv::Mat untouched(10, 10, CV_8UC3, cv::Scalar(1, 2, 3));
  CHECK_FALSE(privacy_mask::apply(untouched, none));
}

TEST_CASE("a masked JPEG is black inside the polygon and keeps the rest of the picture")
{
  const auto masked = privacy_mask::maskedJpeg(whiteJpeg(), leftHalf());
  REQUIRE(masked.has_value());
  const cv::Mat picture = cv::imdecode(masked.value_or(std::vector<uint8_t>{}), cv::IMREAD_COLOR);
  REQUIRE(picture.cols == 80);
  REQUIRE(picture.rows == 40);
  for (const int channel : {0, 1, 2}) {
    CHECK(picture.at<cv::Vec3b>(20, 10)[channel] < 16);
    CHECK(picture.at<cv::Vec3b>(20, 70)[channel] > 239);
  }
}

TEST_CASE("a JPEG that cannot be decoded or masked gives no picture at all")
{
  CHECK_FALSE(privacy_mask::maskedJpeg({}, leftHalf()).has_value());
  CHECK_FALSE(privacy_mask::maskedJpeg({'n', 'o', 't', ' ', 'j', 'p', 'e', 'g'}, leftHalf()).has_value());
  const std::vector<OperatorZone> monitor{{.cameraId = 6,
                                           .name = "Patio",
                                           .kind = ZoneType::Monitor,
                                           .points = {{0.0, 0.0}, {1.0, 0.0}, {1.0, 1.0}}}};
  CHECK_FALSE(privacy_mask::maskedJpeg(whiteJpeg(), monitor).has_value());
}
