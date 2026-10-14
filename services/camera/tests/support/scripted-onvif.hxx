#pragma once

#include <shared/services/onvif/onvif-transport.hxx>

#include <algorithm>
#include <coroutine>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
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

class ScriptedOnvif final : public IOnvifTransport, public IOnvifAsyncTransport
{
public:
  void always(const std::string& operation, OnvifHttpResponse response)
  {
    const std::scoped_lock lock(mutex_);
    always_[operation] = std::move(response);
  }

  void once(const std::string& operation, OnvifHttpResponse response)
  {
    const std::scoped_lock lock(mutex_);
    once_[operation].push_back(std::move(response));
  }

  void hold(const std::string& operation)
  {
    const std::scoped_lock lock(mutex_);
    held_ = operation;
  }

  OnvifHttpResponse post(const OnvifHttpRequest& request) override
  {
    const std::scoped_lock lock(mutex_);
    return answerLocked(request);
  }

  drogon::Task<OnvifHttpResponse> send(OnvifHttpRequest request) override
  {
    const std::string operation = operationOf(request.action);
    bool waits = false;
    OnvifHttpResponse response;
    {
      const std::scoped_lock lock(mutex_);
      ++open_;
      peak_ = std::max(peak_, open_);
      journal_.push_back("open " + operation);
      const auto queued = once_.find(operation);
      waits = operation == held_ && (queued == once_.end() || queued->second.empty());
      if (waits)
        sent.push_back(request);
      else
        response = answerLocked(request);
    }
    if (waits)
      response = co_await Waiting{*this};
    const std::scoped_lock lock(mutex_);
    --open_;
    journal_.push_back("close " + operation);
    co_return response;
  }

  void cancel() override
  {
    std::coroutine_handle<> waiting;
    {
      const std::scoped_lock lock(mutex_);
      journal_.emplace_back("cancel");
      if (!waiting_)
        return;
      waiting = std::exchange(waiting_, {});
      ++cancelled_;
      answer_ = {.status = 0, .body = {}, .challenge = {}, .error = "the pull was cancelled"};
    }
    waiting.resume();
  }

  bool release(OnvifHttpResponse response)
  {
    std::coroutine_handle<> waiting;
    {
      const std::scoped_lock lock(mutex_);
      if (!waiting_)
        return false;
      waiting = std::exchange(waiting_, {});
      answer_ = std::move(response);
    }
    waiting.resume();
    return true;
  }

  [[nodiscard]] std::vector<std::string> operations() const
  {
    const std::scoped_lock lock(mutex_);
    std::vector<std::string> out;
    out.reserve(sent.size());
    for (const auto& request : sent)
      out.push_back(operationOf(request.action));
    return out;
  }

  [[nodiscard]] std::vector<std::string> journal() const
  {
    const std::scoped_lock lock(mutex_);
    return journal_;
  }

  [[nodiscard]] bool waiting() const
  {
    const std::scoped_lock lock(mutex_);
    return static_cast<bool>(waiting_);
  }

  [[nodiscard]] int open() const
  {
    const std::scoped_lock lock(mutex_);
    return open_;
  }

  [[nodiscard]] int peak() const
  {
    const std::scoped_lock lock(mutex_);
    return peak_;
  }

  [[nodiscard]] int cancelled() const
  {
    const std::scoped_lock lock(mutex_);
    return cancelled_;
  }

  std::vector<OnvifHttpRequest> sent;

private:
  struct Waiting
  {
    ScriptedOnvif& owner;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle)
    {
      const std::scoped_lock lock(owner.mutex_);
      owner.waiting_ = handle;
    }

    OnvifHttpResponse await_resume()
    {
      const std::scoped_lock lock(owner.mutex_);
      return std::move(owner.answer_);
    }
  };

  OnvifHttpResponse answerLocked(const OnvifHttpRequest& request)
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

  mutable std::mutex mutex_;
  std::map<std::string, std::deque<OnvifHttpResponse>> once_;
  std::map<std::string, OnvifHttpResponse> always_;
  std::string held_;
  std::coroutine_handle<> waiting_;
  OnvifHttpResponse answer_;
  int open_{0};
  int peak_{0};
  int cancelled_{0};
  std::vector<std::string> journal_;
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
