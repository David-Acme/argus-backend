#include "go2rtc-http.hxx"

#include <shared/services/stream/go2rtc-manager.hxx>

#include <drogon/drogon.h>
#include <trantor/net/EventLoop.h>

#include <unordered_map>

namespace go2rtc_http
{
drogon::HttpClientPtr client(const std::string& lane)
{
  trantor::EventLoop* loop = trantor::EventLoop::getEventLoopOfCurrentThread();
  const std::string base = Go2rtcManager::instance().apiBase();
  if (loop == nullptr)
    return drogon::HttpClient::newHttpClient(base);
  thread_local std::unordered_map<std::string, drogon::HttpClientPtr> clients;
  thread_local std::string clientsBase;
  if (clientsBase != base) {
    clients.clear();
    clientsBase = base;
  }
  auto& cached = clients[lane];
  if (!cached)
    cached = drogon::HttpClient::newHttpClient(base, loop);
  return cached;
}
}
