#pragma once

#include <feature/llm/services/tools/tool-access.hxx>
#include <feature/llm/services/tools/tool-grounding.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <shared/vocabulary/intent-ledger.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <memory>
#include <optional>
#include <vector>

class ToolExecutor
{
public:
  explicit ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

  void attachLedger(std::shared_ptr<tools::IntentLedger> ledger);

  [[nodiscard]] tools::ToolResult execute(const tools::ToolCall& call, const ToolAudience& audience) const;

  [[nodiscard]] std::vector<tools::ToolHandle> offered(const ToolAudience& audience) const;

  [[nodiscard]] std::optional<PendingPreview> pendingPreview(int64_t userId) const { return grounding_.pendingPreview(userId); }

  [[nodiscard]] std::optional<PendingOffer> pendingOffer(int64_t userId) const { return grounding_.pendingOffer(userId); }

  void forgetPending(int64_t userId) const { grounding_.forgetPending(userId); }

private:
  [[nodiscard]] tools::ToolResult inactive(const tools::ToolCall& call, const tools::ToolDescriptor& tool,
                                           const ToolAudience& audience) const;

  ToolRegistry& registry_;
  ToolGrounding grounding_;
  std::shared_ptr<tools::IntentLedger> ledger_;
};
