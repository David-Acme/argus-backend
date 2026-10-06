#include "session-verdict-wire.hxx"

namespace session_verdict_wire
{

void write(const SessionVerdict& verdict,
           argus::auth::v1::ValidateTokenResponse& response)
{
  if (!verdict.valid || !verdict.user.has_value()) {
    response.set_valid(false);
    response.set_reason(verdict.reason);
    return;
  }
  const UserContext& user = *verdict.user;
  auto* payload = response.mutable_user();
  payload->set_user_id(user.userId);
  payload->set_name(user.name);
  payload->set_lang(user.lang);
  payload->set_last_name(user.lastName);
  payload->set_role(user.role);
  payload->set_is_active(user.isActive);
  response.set_expires_at(verdict.expiresAt);
  response.set_session_id(verdict.sessionId);
  response.set_valid(true);
}

}
