#pragma once

#include <drogon/MultiPart.h>
#include <string>
#include <validation/validator.hxx>

struct LoginDto
{
  std::string image;

  static LoginDto form_multipart(const drogon::MultiPartParser& parser);
};
