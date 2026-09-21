#pragma once

#include <drogon/MultiPart.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct RegisterDto
{
  std::string image;
  std::string name;
  std::string inviteCode;
  std::string lang;

  static RegisterDto form_multipart(const drogon::MultiPartParser& parser);
};