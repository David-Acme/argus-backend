#pragma once

#include <shared/services/onvif/onvif-transport.hxx>

#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace scripted_onvif
{
inline std::string fixture(const std::string& name)
{
  std::ifstream in(std::filesystem::path(ARGUS_ONVIF_FIXTURES) / name, std::ios::binary);
  if (!in.good())
    throw std::runtime_error("missing ONVIF fixture " + name);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

inline OnvifHttpResponse answer(const std::string& name, int status = 200)
{
  return {.status = status, .body = fixture(name), .challenge = {}, .error = {}};
}

inline std::string operationOf(const std::string& action)
{
  const auto slash = action.rfind('/');
  return slash == std::string::npos ? action : action.substr(slash + 1);
}

class ScriptedOnvif final : public IOnvifTransport
{
public:
  void always(const std::string& operation, OnvifHttpResponse response)
  {
    always_[operation] = std::move(response);
  }

  void once(const std::string& operation, OnvifHttpResponse response)
  {
    once_[operation].push_back(std::move(response));
  }

  OnvifHttpResponse post(const OnvifHttpRequest& request) override
  {
    sent.push_back(request);
    const std::string operation = operationOf(request.action);
    if (auto queued = once_.find(operation); queued != once_.end() && !queued->second.empty()) {
      OnvifHttpResponse next = std::move(queued->second.front());
      queued->second.pop_front();
      return next;
    }
    if (const auto standing = always_.find(operation); standing != always_.end())
      return standing->second;
    return {.status = 0, .body = {}, .challenge = {}, .error = "no fixture answers " + operation};
  }

  [[nodiscard]] std::vector<std::string> operations() const
  {
    std::vector<std::string> out;
    out.reserve(sent.size());
    for (const auto& request : sent)
      out.push_back(operationOf(request.action));
    return out;
  }

  std::vector<OnvifHttpRequest> sent;

private:
  std::map<std::string, std::deque<OnvifHttpResponse>> once_;
  std::map<std::string, OnvifHttpResponse> always_;
};

inline std::shared_ptr<ScriptedOnvif> ptzCamera()
{
  auto camera = std::make_shared<ScriptedOnvif>();
  camera->always("GetSystemDateAndTime", answer("get-system-date-and-time.xml"));
  camera->always("GetCapabilities", answer("get-capabilities.xml"));
  camera->always("GetDeviceInformation", answer("get-device-information.xml"));
  camera->always("GetProfiles", answer("get-profiles.xml"));
  camera->always("GetPresets", answer("get-presets.xml"));
  camera->always("SetPreset", answer("set-preset.xml"));
  camera->always("GotoPreset", answer("goto-preset.xml"));
  camera->always("RemovePreset", answer("remove-preset.xml"));
  camera->always("ContinuousMove", answer("continuous-move.xml"));
  camera->always("RelativeMove", answer("relative-move.xml"));
  camera->always("Stop", answer("stop.xml"));
  return camera;
}

inline std::shared_ptr<ScriptedOnvif> fixedCamera()
{
  auto camera = std::make_shared<ScriptedOnvif>();
  camera->always("GetSystemDateAndTime", answer("get-system-date-and-time.xml"));
  camera->always("GetCapabilities", answer("get-capabilities-fixed.xml"));
  camera->always("GetDeviceInformation", answer("get-device-information.xml"));
  camera->always("GetProfiles", answer("get-profiles-fixed.xml"));
  return camera;
}
}
