#pragma once

#include <json/value.h>
#include <shared/schemas/user-invitation/user-invitation-schema.hxx>
#include <string>

struct ResponseInvitationDto
{
  UserInvitationSchema invitation;
  std::string token;

  Json::Value toJson() const;
};
