#pragma once

#include <argus/auth/v1/auth.pb.h>
#include <feature/session/services/session-service.hxx>

namespace session_verdict_wire
{

void write(const SessionVerdict& verdict,
           argus::auth::v1::ValidateTokenResponse& response);

}
