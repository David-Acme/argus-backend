#pragma once

#include <feature/llm/services/tools/tool-access.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

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

class ToolGrounding
{
public:
  [[nodiscard]] std::optional<tools::ToolResult> refuse(const GroundingInput& input) const;

  void remember(const GroundingInput& input, const tools::ToolResult& result) const;

private:
  using Key = std::pair<int64_t, std::string>;

  [[nodiscard]] std::optional<tools::ToolResult> refuseConfirmation(const GroundingInput& input) const;
  [[nodiscard]] std::optional<tools::ToolResult> refuseModuleChange(const GroundingInput& input) const;
  [[nodiscard]] std::optional<int64_t> turnOf(const std::map<Key, int64_t>& book, const Key& key) const;

  mutable std::mutex mutex_;
  mutable std::map<Key, int64_t> previews_;
  mutable std::map<Key, int64_t> offers_;
};
