#include <shared/services/onvif/onvif-transport.hxx>

#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>
#include <trantor/net/EventLoop.h>
#include <trantor/utils/Logger.h>

#include <cstdint>
#include <netinet/in.h>
#include <optional>
#include <sys/socket.h>
#include <utility>

namespace
{
drogon::HttpRequestPtr requestOf(const OnvifHttpRequest& request)
{
  auto http = drogon::HttpRequest::newHttpRequest();
  http->setMethod(drogon::Post);
  http->setPath(request.path);
  http->setPathEncode(false);
  http->setContentTypeString("application/soap+xml; charset=utf-8; action=\"" + request.action + "\"");
  http->setBody(request.body);
  if (!request.authorization.empty())
    http->addHeader("Authorization", request.authorization);
  return http;
}

drogon::HttpRequestPtr closingRequestOf(const OnvifHttpRequest& request)
{
  auto http = requestOf(request);
  http->setPassThrough(true);
  http->addHeader("content-type", "application/soap+xml; charset=utf-8; action=\"" + request.action + "\"");
  http->addHeader("content-length", std::to_string(request.body.size()));
  http->addHeader("connection", "close");
  return http;
}

OnvifHttpResponse answerOf(const drogon::HttpResponsePtr& response)
{
  return {.status = static_cast<int>(response->statusCode()),
          .body = std::string(response->body()),
          .challenge = response->getHeader("www-authenticate"),
          .error = {}};
}

OnvifHttpResponse silenceOf(const OnvifEndpoint& endpoint, drogon::ReqResult result)
{
  return {.status = 0,
          .body = {},
          .challenge = {},
          .error = "no ONVIF answer from " + endpoint.host + ":" + std::to_string(endpoint.port) + " (" +
                   drogon::to_string(result) + ")"};
}

double timeoutOf(const OnvifHttpRequest& request, const OnvifEndpoint& endpoint)
{
  return request.timeoutSeconds > 0.0 ? request.timeoutSeconds : endpoint.timeoutSeconds;
}

std::optional<trantor::InetAddress> addressOf(const sockaddr_storage& storage)
{
  if (storage.ss_family == AF_INET)
    return trantor::InetAddress(*reinterpret_cast<const sockaddr_in*>(&storage));
  if (storage.ss_family == AF_INET6)
    return trantor::InetAddress(*reinterpret_cast<const sockaddr_in6*>(&storage));
  return std::nullopt;
}

bool endsItsConnection(drogon::ReqResult result)
{
  return result == drogon::ReqResult::Ok || result == drogon::ReqResult::Timeout;
}
}

class OnvifAsyncHttpTransport::Exchange : public drogon::CallbackAwaiter<OnvifHttpResponse>
{
public:
  Exchange(OnvifAsyncHttpTransport& transport, OnvifHttpRequest request)
      : transport_(transport), request_(std::move(request))
  {
  }

  void await_suspend(std::coroutine_handle<> handle)
  {
    const auto socket = std::make_shared<int>(-1);
    const auto settled = std::make_shared<bool>(false);
    auto client = drogon::HttpClient::newHttpClient(
        transport_.endpoint_.host, static_cast<uint16_t>(transport_.endpoint_.port), false, transport_.loop_);
    client->setSockOptCallback([socket](int fd) { *socket = fd; });
    client->sendRequest(
        closingRequestOf(request_),
        [this, handle, socket, settled](drogon::ReqResult result, const drogon::HttpResponsePtr& response) {
          const bool armed = *socket >= 0 && transport_.pendingFd_ == *socket;
          if (armed)
            transport_.pendingFd_ = -1;
          *settled = true;
          if (result == drogon::ReqResult::Ok && response)
            setValue(answerOf(response));
          else
            setValue(silenceOf(transport_.endpoint_, result));
          if (armed && endsItsConnection(result))
            transport_.close(*socket);
          transport_.loop_->queueInLoop([handle] { handle.resume(); });
        },
        timeoutOf(request_, transport_.endpoint_));
    if (!*settled)
      transport_.pendingFd_ = *socket;
  }

private:
  OnvifAsyncHttpTransport& transport_;
  OnvifHttpRequest request_;
};

OnvifHttpTransport::OnvifHttpTransport(OnvifEndpoint endpoint)
    : endpoint_(std::move(endpoint)),
      client_(drogon::HttpClient::newHttpClient(endpoint_.host, static_cast<uint16_t>(endpoint_.port)))
{
}

OnvifHttpResponse OnvifHttpTransport::post(const OnvifHttpRequest& request)
{
  const auto [result, response] = client_->sendRequest(requestOf(request), timeoutOf(request, endpoint_));
  if (result != drogon::ReqResult::Ok || !response)
    return silenceOf(endpoint_, result);
  return answerOf(response);
}

OnvifAsyncHttpTransport::OnvifAsyncHttpTransport(OnvifEndpoint endpoint, trantor::EventLoop* loop)
    : endpoint_(std::move(endpoint)),
      loop_(loop),
      camera_(endpoint_.host, static_cast<uint16_t>(endpoint_.port), endpoint_.host.find(':') != std::string::npos)
{
}

drogon::Task<OnvifHttpResponse> OnvifAsyncHttpTransport::send(OnvifHttpRequest request)
{
  const auto keep = shared_from_this();
  co_await drogon::switchThreadCoro(loop_);
  co_return co_await Exchange(*this, std::move(request));
}

void OnvifAsyncHttpTransport::cancel()
{
  if (!loop_->isInLoopThread()) {
    loop_->queueInLoop([self = shared_from_this()] { self->cancel(); });
    return;
  }
  const int fd = std::exchange(pendingFd_, -1);
  if (fd >= 0)
    close(fd);
}

int64_t OnvifAsyncHttpTransport::closedConnections() const
{
  return closed_.load();
}

void OnvifAsyncHttpTransport::close(int fd)
{
  if (!reachesCamera(fd)) {
    if (!std::exchange(strangerLogged_, true))
      LOG_WARN << "ONVIF events: socket " << fd << " does not lead to " << endpoint_.host << ":" << endpoint_.port
               << "; it is left open";
    return;
  }
  ::shutdown(fd, SHUT_RDWR);
  ++closed_;
}

bool OnvifAsyncHttpTransport::reachesCamera(int fd) const
{
  if (camera_.isUnspecified())
    return false;
  sockaddr_storage local{};
  socklen_t localLength = sizeof(local);
  sockaddr_storage peer{};
  socklen_t peerLength = sizeof(peer);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localLength) != 0 ||
      ::getpeername(fd, reinterpret_cast<sockaddr*>(&peer), &peerLength) != 0 || local.ss_family != peer.ss_family)
    return false;
  const auto address = addressOf(peer);
  return address && address->toPort() == camera_.toPort() && address->toIp() == camera_.toIp();
}
