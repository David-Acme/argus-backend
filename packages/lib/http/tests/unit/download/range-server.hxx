#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

struct ServedRequest
{
  std::string path;
  std::optional<std::uint64_t> rangeFirst;
  std::optional<std::uint64_t> rangeLast;
};

struct RangeServerOptions
{
  std::string content;
  bool honourRange = true;
  int dropOnFileRequest = -1;
};

class RangeServer
{
public:
  explicit RangeServer(RangeServerOptions options);
  RangeServer(const RangeServer&) = delete;
  RangeServer& operator=(const RangeServer&) = delete;
  RangeServer(RangeServer&&) = delete;
  RangeServer& operator=(RangeServer&&) = delete;
  ~RangeServer();

  [[nodiscard]] std::string url(std::string_view path) const;
  [[nodiscard]] std::vector<ServedRequest> requests() const;
  [[nodiscard]] std::vector<ServedRequest> fileRequests() const;

private:
  void acceptLoop();
  void serveConnection(int connection);
  [[nodiscard]] bool answer(int connection, const ServedRequest& request);

  RangeServerOptions options_;
  int listener_ = -1;
  std::uint16_t port_ = 0;
  mutable std::mutex mutex_;
  bool stopping_ = false;
  int fileRequests_ = 0;
  std::vector<int> connections_;
  std::vector<ServedRequest> requests_;
  std::vector<std::thread> workers_;
  std::thread acceptor_;
};
