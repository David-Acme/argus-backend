#pragma once

#include <drogon/MultiPart.h>
#include <string>
#include <validation/validation_dsl.hxx>
#include <vector>

struct EnrollVoiceprintDto
{
  std::vector<std::string> samples;
  std::string face;
  std::string consent;
  std::string consentVersion;
  std::string challengeId;

  static EnrollVoiceprintDto
  form_multipart(const drogon::MultiPartParser& parser);
};
