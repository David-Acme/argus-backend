#pragma once

#include <llm/llm-service.hxx>

#include <functional>
#include <string>
#include <string_view>

namespace reply_claims
{

struct Reply
{
  std::string_view text;
  bool asked{false};
  bool appOnly{false};
  std::string_view lang{};
};

[[nodiscard]] bool claimsDone(const Reply& reply);

[[nodiscard]] bool asksForAction(std::string_view utterance);

struct Plain
{
  std::string text;
  std::string_view utterance;
  std::string_view lang;
};

[[nodiscard]] std::string withoutFalseClaims(Plain plain);

[[nodiscard]] std::string honest(std::string_view lang);

[[nodiscard]] std::string nudge(std::string_view lang);

struct GateInput
{
  TokenCallback sink;
  std::string lang;
  bool asked{false};
  std::function<bool()> legitimate;
  bool appOnly{false};
};

class ClaimGate
{
public:
  explicit ClaimGate(GateInput input);

  ClaimGate(const ClaimGate&) = delete;
  ClaimGate& operator=(const ClaimGate&) = delete;

  [[nodiscard]] TokenCallback callback();

  [[nodiscard]] bool cut() const noexcept { return cut_; }

  [[nodiscard]] const std::string& spoken() const noexcept { return spoken_; }

private:
  void accept(const std::string& token, bool done);
  void release(const std::string& sentence);
  void finish();

  GateInput input_;
  std::string pending_;
  std::string spoken_;
  bool cut_{false};
  bool finished_{false};
};

}
