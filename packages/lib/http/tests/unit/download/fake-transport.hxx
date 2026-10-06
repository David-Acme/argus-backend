#pragma once

#include <cstdint>
#include <functional>
#include <http/download/chunk-transport.hxx>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct FakeCall
{
  int index = 0;
  std::string url;
  std::uint64_t first = 0;
  std::uint64_t last = 0;
};

class FakeTransport final : public file_download::ChunkTransport
{
public:
  static constexpr std::string_view kHubUrl = "https://hub.test/org/repo/resolve/rev/model.bin";
  static constexpr std::string_view kDirectUrl = "https://files.test/model.bin";

  explicit FakeTransport(std::string content) : content_(std::move(content)) {}

  [[nodiscard]] file_download::ChunkResponse fetch(const file_download::ChunkRequest& request) override;

  std::function<std::optional<file_download::ChunkResponse>(const FakeCall&)> intercept;

  void expireCdn();
  void setEtag(std::string etag);
  void setIgnoreRange(bool ignore);
  [[nodiscard]] std::vector<FakeCall> calls() const;

  [[nodiscard]] static file_download::ChunkResponse status(int code);
  [[nodiscard]] static file_download::ChunkResponse error(file_download::TransportError kind);

private:
  [[nodiscard]] file_download::ChunkResponse serve(const FakeCall& call) const;

  mutable std::mutex mutex_;
  std::string content_;
  std::string etag_ = "\"v1\"";
  bool ignoreRange_ = false;
  int generation_ = 1;
  std::vector<FakeCall> calls_;
};
