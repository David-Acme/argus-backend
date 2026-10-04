#include "camera-talk-service.hxx"

#include <auth/jwt-filter.hxx>
#include <auth/role-access.hxx>
#include <camera/camera-errors.hxx>
#include <errors/response-exception.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>

#include <algorithm>
#include <array>
#include <string_view>

namespace
{
constexpr std::string_view kStart = "camera:talk:start";
constexpr std::string_view kStop = "camera:talk:stop";
constexpr std::array kSampleRates{8000, 16000, 24000, 32000, 44100, 48000};

void sendIfOpen(const std::weak_ptr<drogon::WebSocketConnection>& weak, const Json::Value& message)
{
  const auto conn = weak.lock();
  if (conn && conn->connected())
    conn->sendJson(message);
}
}

CameraTalkService::CameraTalkService(TalkLimits limits) : limits_(limits) {}

CameraTalkService::~CameraTalkService()
{
  requestStop();
  std::vector<std::shared_ptr<CameraTalkSession>> all;
  {
    std::scoped_lock lock(mutex_);
    for (auto& [key, session] : sessions_)
      all.push_back(session);
    all.insert(all.end(), retired_.begin(), retired_.end());
    sessions_.clear();
    retired_.clear();
  }
  for (const auto& session : all)
    session->join();
}

bool CameraTalkService::handles(const std::string& type)
{
  return type.starts_with("camera:talk:");
}

drogon::Task<bool> CameraTalkService::handleText(const SyncFrameInput& input)
{
  const std::string type = input.message["type"].asString();
  if (type == kStart) {
    co_await start(input);
    co_return true;
  }
  if (type == kStop) {
    stopConnection(input.conn, "stopped");
    co_return true;
  }
  co_return false;
}

drogon::Task<void> CameraTalkService::start(const SyncFrameInput& input)
{
  const auto& conn = input.conn;
  const auto& ctx = conn->getContextRef<JwtContext>();
  if (!role_access::hasCameraAction(ctx.role, role_access::CameraAction::Talk))
    throw ResponseException(403, CameraErrors::Forbidden);

  const Json::Value& payload = input.message["payload"];
  const int64_t cameraId = payload.get("cameraId", 0).asInt64();
  if (cameraId <= 0)
    throw ResponseException(400, CameraErrors::InvalidCameraId);
  const int sampleRate = payload.get("sampleRate", 16000).asInt();
  if (std::ranges::find(kSampleRates, sampleRate) == kSampleRates.end())
    throw ResponseException(CameraErrors::InvalidTalkFormat);

  const auto camera = co_await cameraRepository_.findById(cameraId);
  if (!camera)
    throw ResponseException(404, CameraErrors::CameraNotFound);
  if (!camera->isEnabled)
    throw ResponseException(CameraErrors::CameraDisabled);
  if (conn->disconnected())
    co_return;

  const auto driver = CameraDriverRegistry::instance().driverFor(*camera);
  if (!driver || !driver->capabilities().get("talk", false).asBool())
    throw ResponseException(CameraErrors::TalkUnavailable);

  std::shared_ptr<CameraTalkSession> session;
  {
    std::scoped_lock lock(mutex_);
    if (stopping_)
      throw ResponseException(503, CameraErrors::SubscribeFailed.withMessage("shutting down"));
    if (const auto mine = sessions_.find(conn.get()); mine != sessions_.end()) {
      mine->second->stop("replaced");
      retired_.push_back(mine->second);
      sessions_.erase(mine);
    }
    const bool busy = std::ranges::any_of(sessions_, [cameraId](const auto& entry) {
      return entry.second->cameraId() == cameraId && !entry.second->finished();
    });
    if (busy)
      throw ResponseException(CameraErrors::TalkLineBusy);
    if (sessions_.size() >= limits_.maxSessions)
      throw ResponseException(429, CameraErrors::TooManyCameraSubscriptions);

    const std::weak_ptr<drogon::WebSocketConnection> weak = conn;
    session = std::make_shared<CameraTalkSession>(
        TalkSessionConfig{.cameraId = cameraId,
                          .sampleRate = sampleRate,
                          .packetMs = 120,
                          .idleMs = limits_.idleMs,
                          .maxMs = limits_.maxMs,
                          .maxQueuedMs = 1000},
        driver,
        TalkSessionEvents{.send = [weak](const Json::Value& message) { sendIfOpen(weak, message); }});
    sessions_[conn.get()] = session;
  }
  session->start();
  reap();
}

void CameraTalkService::handleBinary(const drogon::WebSocketConnectionPtr& conn,
                                     std::span<const uint8_t> data)
{
  const auto samples = talk_frame::parse(data);
  if (!samples)
    return;
  std::shared_ptr<CameraTalkSession> session;
  {
    std::scoped_lock lock(mutex_);
    const auto found = sessions_.find(conn.get());
    if (found == sessions_.end())
      return;
    session = found->second;
  }
  session->push(*samples);
}

void CameraTalkService::handleClose(const drogon::WebSocketConnectionPtr& conn)
{
  stopConnection(conn, "socket_closed");
}

void CameraTalkService::stopConnection(const drogon::WebSocketConnectionPtr& conn,
                                       const std::string& reason)
{
  {
    std::scoped_lock lock(mutex_);
    const auto found = sessions_.find(conn.get());
    if (found != sessions_.end()) {
      found->second->stop(reason);
      retired_.push_back(found->second);
      sessions_.erase(found);
    }
  }
  reap();
}

void CameraTalkService::reap()
{
  std::vector<std::shared_ptr<CameraTalkSession>> done;
  {
    std::scoped_lock lock(mutex_);
    std::erase_if(sessions_, [&done](const auto& entry) {
      if (!entry.second->finished())
        return false;
      done.push_back(entry.second);
      return true;
    });
    std::erase_if(retired_, [&done](const auto& session) {
      if (!session->finished())
        return false;
      done.push_back(session);
      return true;
    });
  }
  for (const auto& session : done)
    session->join();
}

void CameraTalkService::requestStop()
{
  std::scoped_lock lock(mutex_);
  stopping_ = true;
  for (auto& [key, session] : sessions_)
    session->stop("shutdown");
}

bool CameraTalkService::drained() const
{
  std::scoped_lock lock(mutex_);
  const auto finished = [](const auto& session) { return session->finished(); };
  return std::ranges::all_of(sessions_, [&finished](const auto& entry) { return finished(entry.second); }) &&
         std::ranges::all_of(retired_, finished);
}

size_t CameraTalkService::active() const
{
  std::scoped_lock lock(mutex_);
  return static_cast<size_t>(std::ranges::count_if(
      sessions_, [](const auto& entry) { return !entry.second->finished(); }));
}
