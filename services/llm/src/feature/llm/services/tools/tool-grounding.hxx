#pragma once

#include <feature/llm/services/tools/tool-access.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

struct GroundingInput
{
  const tools::ToolCall& call;
  const argus::mcp::ToolSpec& spec;
  const ToolAudience& audience;
};

struct PendingPreview
{
  std::string tool;
  Json::Value arguments{Json::objectValue};
  int64_t turn{0};
  std::chrono::steady_clock::time_point at{};
};

struct PendingOffer
{
  std::string module;
  int64_t turn{0};
  std::chrono::steady_clock::time_point at{};
};

class ToolGrounding
{
public:
  [[nodiscard]] std::optional<tools::ToolResult> refuse(const GroundingInput& input) const;

  void remember(const GroundingInput& input, const tools::ToolResult& result) const;

  [[nodiscard]] std::optional<PendingPreview> pendingPreview(int64_t userId) const;

  [[nodiscard]] std::optional<PendingOffer> pendingOffer(int64_t userId) const;

  void forgetPending(int64_t userId) const;

private:
  using Key = std::pair<int64_t, std::string>;

  [[nodiscard]] std::optional<tools::ToolResult> refuseConfirmation(const GroundingInput& input) const;
  [[nodiscard]] std::optional<tools::ToolResult> refuseModuleChange(const GroundingInput& input) const;
  [[nodiscard]] std::optional<int64_t> turnOf(const std::map<Key, int64_t>& book, const Key& key) const;

  mutable std::mutex mutex_;
  mutable std::map<Key, int64_t> previews_;
  mutable std::map<Key, int64_t> offers_;

  struct Previewed
  {
    PendingPreview preview;
    std::chrono::steady_clock::time_point at;
  };

  struct Offered
  {
    PendingOffer offer;
    std::chrono::steady_clock::time_point at;
  };

  mutable std::map<int64_t, Previewed> latestPreview_;
  mutable std::map<int64_t, Offered> latestOffer_;
};
