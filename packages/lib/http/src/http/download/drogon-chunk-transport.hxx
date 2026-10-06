#pragma once

#include <http/download/chunk-transport.hxx>
#include <map>
#include <memory>
#include <string>

namespace drogon
{
class HttpClient;
}

namespace trantor
{
class EventLoopThread;
}

namespace file_download
{

class DrogonChunkTransport final : public ChunkTransport
{
public:
  DrogonChunkTransport();
  DrogonChunkTransport(const DrogonChunkTransport&) = delete;
  DrogonChunkTransport& operator=(const DrogonChunkTransport&) = delete;
  DrogonChunkTransport(DrogonChunkTransport&&) = delete;
  DrogonChunkTransport& operator=(DrogonChunkTransport&&) = delete;
  ~DrogonChunkTransport() override;

  [[nodiscard]] ChunkResponse fetch(const ChunkRequest& request) override;

private:
  std::shared_ptr<drogon::HttpClient> clientFor(const std::string& origin);
  void forget(const std::string& origin);

  std::unique_ptr<trantor::EventLoopThread> loopThread_;
  std::map<std::string, std::shared_ptr<drogon::HttpClient>, std::less<>> clients_;
};

}
