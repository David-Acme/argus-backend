#pragma once

#include <feature/privacy/services/privacy-feature-service.hxx>
#include <json/value.h>

struct ResponsePrivacyDto
{
  PrivacyView view;

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponsePrivacyDirectoryDto
{
  PrivacyDirectory directory;

  [[nodiscard]] Json::Value toJson() const;
};
