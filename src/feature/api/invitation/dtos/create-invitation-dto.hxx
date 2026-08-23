#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/enums.hxx>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CreateInvitationDto
{
  UserRole role{UserRole::Guest};
  std::string roleValue;
  int maxRedemptions{1};
  int64_t expiresAt{0};

  static CreateInvitationDto fromJson(const Json::Value& json);
};
