#include "claim-check.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/reply-claims.hxx>

bool claimedWithoutTool(const std::string& reply, const TurnState& state)
{
  if (state.wrote)
    return false;
  if (state.opened)
    return reply_claims::claimsDone({.text = reply, .asked = state.asked, .appOnly = true, .lang = state.lang});
  return reply_claims::claimsDone({.text = reply, .asked = state.asked, .appOnly = false, .lang = state.lang}) ||
         (state.appAsked && claimsAppAction(reply));
}
