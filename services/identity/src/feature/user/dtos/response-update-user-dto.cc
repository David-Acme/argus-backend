#include "response-update-user-dto.hxx"

Json::Value ResponseUpdateUserDto::toJson() const
{
  Json::Value json = user.toJson();
  json["roleActive"] = roleActive;
  return json;
}
