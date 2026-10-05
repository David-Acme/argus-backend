#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/services/privacy/camera-audio-policy.hxx>

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
