#pragma once

#include <drogon/MultiPart.h>
#include <string>
#include <validation/validation_dsl.hxx>

struct RegisterDto
{
  std::string image;
  std::string name;
  std::string inviteCode;
  std::string lang;

  static RegisterDto form_multipart(const drogon::MultiPartParser& parser);
};
