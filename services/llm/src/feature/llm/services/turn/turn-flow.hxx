#pragma once

#include "decider.hxx"
#include "decision-policy.hxx"
#include "decision-tally.hxx"
#include "pending-turn.hxx"
#include "slots.hxx"

#include <feature/llm/services/tools/tool-access.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace turn
{

enum class FindingKind : unsigned char
{
  Done,
  Refused,
  Preview,
  Offer,
  Declined,
  Unactionable
};

struct Finding
{
  FindingKind kind{FindingKind::Done};
  std::string tool;
  std::string text;
};

struct Step
{
  tools::ToolCall call;
  tools::ToolResult result;
};

struct Outcome
{
  std::vector<Step> steps;
  std::vector<Finding> findings;
  std::optional<std::string> question;
  bool wrote{false};
  int64_t toolMs{0};
  std::string source;
};

struct TurnRequest
{
  std::string_view utterance;
  const std::vector<tools::ToolHandle>& offered;
  const ToolAudience& audience;
  const tools::ToolContext& context;
  int64_t now{0};
};

struct FlowDeps
{
  ToolExecutor& executor;
  const Decider* decider{nullptr};
  const slots::TextSlots* text{nullptr};
  PolicySet policies;
};

struct SecondOpinion
{
  const Candidate& candidate;
  const DecideInput& input;
};

using SecondSignal = std::function<bool(const SecondOpinion&)>;

class TurnFlow
{
public:
  explicit TurnFlow(FlowDeps deps);

  void useDecider(const Decider& decider) { decider_ = &decider; }

  void useText(const slots::TextSlots& text) { text_ = &text; }

  void usePolicies(PolicySet policies) { policies_ = std::move(policies); }

  void useWitnesses(std::vector<const Decider*> witnesses) { witnesses_ = std::move(witnesses); }

  void useSecondSignal(SecondSignal signal) { secondSignal_ = std::move(signal); }

  [[nodiscard]] Outcome run(const TurnRequest& request);

  [[nodiscard]] static std::string notes(const Outcome& outcome, std::string_view lang);

  [[nodiscard]] std::vector<DecisionCount> decisions() const { return tally_.snapshot(); }

private:
  class Deciding;
  struct Move;

  [[nodiscard]] std::optional<Outcome> followUp(const TurnRequest& request, const Deciding& deciding);
  [[nodiscard]] std::optional<Outcome> followUpOwn(const TurnRequest& request, const Deciding& deciding, const Pending& pending);
  [[nodiscard]] std::optional<Outcome> followUpPreview(const TurnRequest& request, const Deciding& deciding, const PendingPreview& preview);
  [[nodiscard]] std::optional<Outcome> followUpOffer(const TurnRequest& request, const Deciding& deciding, const PendingOffer& offer);
  [[nodiscard]] Outcome decided(const TurnRequest& request, const Deciding& deciding);
  [[nodiscard]] Outcome confirm(const TurnRequest& request, const Candidate& candidate);
  [[nodiscard]] Outcome choose(const TurnRequest& request, const Candidate& candidate);
  [[nodiscard]] bool corroborated(const SecondOpinion& opinion) const;
  [[nodiscard]] Verdict levelOf(const Candidate& candidate) const;
  [[nodiscard]] Verdict verdictOf(const Candidate& candidate) const;
  [[nodiscard]] bool needsSecondSignal(const Candidate& candidate, const tools::ToolDescriptor& tool) const;
  [[nodiscard]] bool supersedes(const Deciding& deciding) const;
  [[nodiscard]] Outcome proceed(const Move& move);
  [[nodiscard]] Outcome ask(const Move& move);
  [[nodiscard]] Outcome unactionable(const TurnRequest& request) const;
  [[nodiscard]] Outcome declined() const;
  void execute(const TurnRequest& request, tools::ToolCall call, Outcome& outcome);
  void clearAll(int64_t userId);

  ToolExecutor& executor_;
  const Decider* decider_;
  const slots::TextSlots* text_;
  PolicySet policies_;
  std::vector<const Decider*> witnesses_;
  SecondSignal secondSignal_;
  PendingTurns pendings_;
  DecisionTally tally_;
};

}
