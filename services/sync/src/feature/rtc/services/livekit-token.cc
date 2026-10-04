#include "livekit-token.hxx"

#include <jwt-cpp/traits/nlohmann-json/defaults.h>

std::string mintLiveKitToken(const LiveKitTokenInput& input)
{
  nlohmann::json video = nlohmann::json::object();
  const LiveKitGrant& grant = input.grant;
  if (!grant.room.empty())
    video["room"] = grant.room;
  if (grant.roomJoin)
    video["roomJoin"] = true;
  if (grant.roomAdmin)
    video["roomAdmin"] = true;
  if (grant.roomList)
    video["roomList"] = true;
  if (grant.roomJoin) {
    video["canPublish"] = grant.canPublish;
    video["canSubscribe"] = grant.canSubscribe;
    video["canPublishData"] = grant.canPublishData;
    if (!grant.publishSources.empty())
      video["canPublishSources"] = grant.publishSources;
    if (grant.canUpdateOwnMetadata)
      video["canUpdateOwnMetadata"] = true;
  }

  auto builder = jwt::create()
                     .set_type("JWT")
                     .set_issuer(std::string(input.apiKey))
                     .set_not_before(input.now - std::chrono::seconds(5))
                     .set_issued_at(input.now)
                     .set_expires_at(input.now + input.ttl)
                     .set_payload_claim("video", jwt::claim(video));
  if (!input.identity.empty())
    builder.set_subject(input.identity);
  if (!input.kind.empty())
    builder.set_payload_claim("kind", jwt::claim(input.kind));
  return builder.sign(jwt::algorithm::hs256{std::string(input.apiSecret)});
}
