#pragma once

#include <drogon/MultiPart.h>
#include <shared/validation/validator.hxx>
#include <string>

struct LoginDto
{
  std::string image;

  static LoginDto form_multipart(const drogon::MultiPartParser& parser);
};
