#pragma once

#include "eval-score.hxx"

#include <mcp/confirmation.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace eval
{

class Recorder
{
public:
  void clear();
  void ran(const RecordedCall& call);
  void previewed(const std::string& tool);
  void confirmed(const std::string& tool);

  [[nodiscard]] std::vector<RecordedCall> executed() const;
  [[nodiscard]] std::vector<std::string> previews() const;
  [[nodiscard]] std::vector<std::string> confirmations() const;

private:
  mutable std::mutex mutex_;
  std::vector<RecordedCall> executed_;
  std::vector<std::string> previews_;
  std::vector<std::string> confirmed_;
};

struct StubInput
{
  std::shared_ptr<Recorder> recorder;
  std::shared_ptr<argus::mcp::ConfirmationLedger> ledger;
};

std::vector<tools::ToolDescriptor> stubTools(const StubInput& input);

}
