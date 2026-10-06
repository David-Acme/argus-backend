#include "drogon-chunk-transport.hxx"

#include "details/download-url.hxx"
#include "details/peer-name.hxx"

#include <condition_variable>
#include <drogon/HttpClient.h>
#include <mutex>
#include <semaphore>
#include <trantor/net/EventLoopThread.h>
#include <utility>

namespace file_download
{
namespace
{

using namespace std::chrono_literals;

constexpr auto kCancelPoll = 20ms;

struct PendingExchange
{
  std::mutex mutex;
  std::condition_variable ready;
  bool done = false;
  drogon::ReqResult result = drogon::ReqResult::Ok;
  drogon::HttpResponsePtr response;
};

TransportError errorOf(drogon::ReqResult result)
{
  switch (result)
  {
  case drogon::ReqResult::Ok:
    return TransportError::None;
  case drogon::ReqResult::Timeout:
    return TransportError::Timeout;
  case drogon::ReqResult::HandshakeError:
  case drogon::ReqResult::InvalidCertificate:
  case drogon::ReqResult::EncryptionFailure:
    return TransportError::Tls;
  case drogon::ReqResult::BadResponse:
  case drogon::ReqResult::NetworkFailure:
  case drogon::ReqResult::BadServerAddress:
    return TransportError::Network;
  }
  return TransportError::Network;
}

std::string_view describe(drogon::ReqResult result)
{
  switch (result)
  {
  case drogon::ReqResult::Ok:
    return "ok";
  case drogon::ReqResult::BadResponse:
    return "bad response";
  case drogon::ReqResult::NetworkFailure:
    return "network failure";
  case drogon::ReqResult::BadServerAddress:
    return "bad server address";
  case drogon::ReqResult::Timeout:
    return "timeout";
  case drogon::ReqResult::HandshakeError:
    return "tls handshake error";
  case drogon::ReqResult::InvalidCertificate:
    return "invalid certificate";
  case drogon::ReqResult::EncryptionFailure:
    return "encryption failure";
  }
  return "unknown";
}

ChunkResponse failed(TransportError error, std::string detail)
{
  return {.error = error,
          .status = 0,
          .contentRange = {},
          .etag = {},
          .location = {},
          .body = {},
          .detail = std::move(detail)};
}

bool namesHost(const drogon::HttpResponse& response, std::string_view origin)
{
  const auto& certificate = response.peerCertificate();
  return certificate &&
         details::certificateNamesHost({.certificatePem = certificate->pem(), .origin = origin});
}

drogon::HttpRequestPtr rangeRequest(const ChunkRequest& request, const std::string& target)
{
  auto http = drogon::HttpRequest::newHttpRequest();
  http->setMethod(drogon::Get);
  http->setPathEncode(false);
  http->setPath(target);
  http->addHeader("Range", "bytes=" + std::to_string(request.first) + "-" +
                               std::to_string(request.last));
  http->addHeader("Accept-Encoding", "identity");
  return http;
}

}

DrogonChunkTransport::DrogonChunkTransport()
    : loopThread_(std::make_unique<trantor::EventLoopThread>("argus-download"))
{
  loopThread_->run();
}

DrogonChunkTransport::~DrogonChunkTransport()
{
  std::binary_semaphore released{0};
  loopThread_->getLoop()->runInLoop(
      [clients = std::move(clients_), &released]() mutable
      {
        clients.clear();
        released.release();
      });
  released.acquire();
  loopThread_.reset();
}

std::shared_ptr<drogon::HttpClient> DrogonChunkTransport::clientFor(const std::string& origin)
{
  if (const auto found = clients_.find(origin); found != clients_.end())
    return found->second;
  auto client = drogon::HttpClient::newHttpClient(origin, loopThread_->getLoop(), false, true);
  client->setUserAgent("Argus");
  clients_.emplace(origin, client);
  return client;
}

void DrogonChunkTransport::forget(const std::string& origin)
{
  const auto found = clients_.find(origin);
  if (found == clients_.end())
    return;
  loopThread_->getLoop()->queueInLoop([client = std::move(found->second)]() mutable { client.reset(); });
  clients_.erase(found);
}

ChunkResponse DrogonChunkTransport::fetch(const ChunkRequest& request)
{
  const auto parts = details::splitUrl(request.url);
  if (!parts)
    return failed(TransportError::InvalidUrl, "not an http(s) url");
  if (request.cancellation.cancelled())
    return failed(TransportError::Cancelled, "cancelled");
  auto pending = std::make_shared<PendingExchange>();
  const auto seconds = std::chrono::duration<double>(request.timeout).count();
  const auto client = clientFor(parts->origin);
  client->sendRequest(
          rangeRequest(request, parts->target),
          [pending](drogon::ReqResult result, const drogon::HttpResponsePtr& response)
          {
            const std::scoped_lock lock(pending->mutex);
            pending->result = result;
            pending->response = response;
            pending->done = true;
            pending->ready.notify_all();
          },
          seconds);
  std::unique_lock lock(pending->mutex);
  while (!pending->done)
  {
    if (request.cancellation.cancelled())
    {
      lock.unlock();
      forget(parts->origin);
      return failed(TransportError::Cancelled, "cancelled");
    }
    pending->ready.wait_for(lock, kCancelPoll);
  }
  if (pending->result != drogon::ReqResult::Ok || !pending->response)
  {
    const auto result = pending->result;
    lock.unlock();
    forget(parts->origin);
    return failed(errorOf(result), std::string(describe(result)));
  }
  const auto& response = *pending->response;
  if (parts->origin.starts_with("https://") && !namesHost(response, parts->origin))
  {
    lock.unlock();
    forget(parts->origin);
    return failed(TransportError::Tls, "the certificate does not name " + details::hostOfOrigin(parts->origin));
  }
  return {.error = TransportError::None,
          .status = static_cast<int>(response.statusCode()),
          .contentRange = response.getHeader("content-range"),
          .etag = response.getHeader("etag"),
          .location = response.getHeader("location"),
          .body = std::string(response.body()),
          .detail = {}};
}

}
