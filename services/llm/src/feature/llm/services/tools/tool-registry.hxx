#pragma once

#include <mcp/client.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

struct ToolProvider
{
  std::string id;
  std::shared_ptr<argus::mcp::McpClient> client;
};

struct RefreshOutcome
{
  std::vector<std::string> refreshed;
  std::vector<std::string> failed;
};

class ToolRegistry
{
public:
  static ToolRegistry& instance();

  ToolRegistry() = default;

  void registerTool(tools::ToolDescriptor descriptor);
  void addProvider(ToolProvider provider);
  RefreshOutcome refresh(std::string_view only = {});

  [[nodiscard]] tools::ToolHandle find(const std::string& name) const;
  [[nodiscard]] std::vector<tools::ToolHandle> all() const;
  [[nodiscard]] std::vector<std::string> names() const;
  [[nodiscard]] std::vector<std::string> unlisted() const;

private:
  struct ProviderState
  {
    ToolProvider provider;
    std::vector<tools::ToolHandle> tools;
    bool listed{false};
  };

  void rebuild();

  std::vector<tools::ToolHandle> local_;
  std::vector<ProviderState> providers_;
  std::vector<tools::ToolHandle> merged_;
  mutable std::shared_mutex mutex_;
};
