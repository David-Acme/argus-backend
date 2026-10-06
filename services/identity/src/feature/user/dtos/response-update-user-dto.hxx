#pragma once

#include <json/value.h>
#include <shared/schemas/user/user-schema.hxx>

struct ResponseUpdateUserDto
{
  UserSchema user;
  bool roleActive{true};

  Json::Value toJson() const;
};
