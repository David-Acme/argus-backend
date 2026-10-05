#pragma once

#include <cstdint>
#include <json/value.h>
#include <string>
#include <vector>

struct ResponseRecipientDto
{
  int64_t userId{0};
  std::string mode;
  int step{0};
  bool onDuty{false};
};

struct ResponseContactDto
{
  std::string name;
  std::string phone;
  std::string note;
};

struct UpdateResponseDto
{
  std::string emergencyNumber;
  int stepSeconds{45};
  std::vector<ResponseRecipientDto> recipients;
  std::vector<ResponseContactDto> contacts;
  std::string recipientsError;
  std::string contactsError;
  std::string typeError;

  static UpdateResponseDto fromJson(const Json::Value& json);
};
