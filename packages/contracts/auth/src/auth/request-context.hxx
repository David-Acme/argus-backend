#pragma once

#include <string>

// The attribute keys the auth boundary writes and every authenticated handler
// reads. They live in the contract, not in the filter that produces them: the
// producer and the fifty readers must agree on the spelling, and a key that
// lives in one side's header is the duplication that breaks the first time the
// two disagree.
//
// The types stored under them (JwtContext, DeviceContext) belong to the auth
// package; the keys travel through here so a consumer needs no Drogon.
namespace AuthContext
{
// JwtContext: the subject the token filter resolved.
inline const std::string kJwtKey{"jwt_ctx"};
// DeviceContext: the fingerprint the device filter computed for this request.
inline const std::string kDeviceKey{"device_ctx"};
} // namespace AuthContext
