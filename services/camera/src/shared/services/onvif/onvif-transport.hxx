#pragma once

#include <shared/services/onvif/onvif-types.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/HttpClient.h>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>
#include <trantor/net/InetAddress.h>

namespace trantor
{
class EventLoop;
}

struct OnvifHttpRequest
{
  std::string path;
  std::string action;
  std::string body;
  std::string authorization;
  double timeoutSeconds{0.0};
};

struct OnvifHttpResponse
{
  int status{0};
  std::string body;
  std::string challenge;
  std::string error;
};

class IOnvifTransport
{
public:
  virtual ~IOnvifTransport() = default;
  virtual OnvifHttpResponse post(const OnvifHttpRequest& request) = 0;
};

class IOnvifAsyncTransport
{
public:
  virtual ~IOnvifAsyncTransport() = default;
  virtual drogon::Task<OnvifHttpResponse> send(OnvifHttpRequest request) = 0;
  virtual void cancel() = 0;
};

class OnvifHttpTransport final : public IOnvifTransport
{
public:
  explicit OnvifHttpTransport(OnvifEndpoint endpoint);

  OnvifHttpResponse post(const OnvifHttpRequest& request) override;

private:
  OnvifEndpoint endpoint_;
  drogon::HttpClientPtr client_;
};

class OnvifAsyncHttpTransport final : public IOnvifAsyncTransport,
                                      public std::enable_shared_from_this<OnvifAsyncHttpTransport>
{
public:
  OnvifAsyncHttpTransport(OnvifEndpoint endpoint, trantor::EventLoop* loop);

  drogon::Task<OnvifHttpResponse> send(OnvifHttpRequest request) override;
  void cancel() override;

  [[nodiscard]] int64_t closedConnections() const;

private:
  class Exchange;

  void close(int fd);
  [[nodiscard]] bool reachesCamera(int fd) const;

  OnvifEndpoint endpoint_;
  trantor::EventLoop* loop_;
  trantor::InetAddress camera_;
  int pendingFd_{-1};
  bool strangerLogged_{false};
  std::atomic<int64_t> closed_{0};
};
