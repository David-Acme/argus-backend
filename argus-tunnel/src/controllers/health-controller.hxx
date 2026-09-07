#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <json/value.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// /health provider wired to either tunnel binary's live link state. The push
// accessors are optional: the client reports queue/Received/Dropped, the
// relay also reports Forwarded; a binary without push wiring leaves them
// unset.
struct HealthStatus
{
  std::string serviceName;
  std::function<bool()> homeConnected;
  std::function<int()> activeStreams;
  std::function<std::size_t()> pushQueued;
  std::function<std::uint64_t()> pushReceived;
  std::function<std::uint64_t()> pushDropped;
  std::function<std::uint64_t()> pushForwarded;
};

class HealthController
    : public drogon::HttpController<HealthController, false>
{
public:
  explicit HealthController(HealthStatus status);

  METHOD_LIST_BEGIN
  ADD_METHOD_TO(HealthController::health, "/health", drogon::Get);
  METHOD_LIST_END

  drogon::Task<drogon::HttpResponsePtr> health(drogon::HttpRequestPtr req);

private:
  HealthStatus status_;
};
