#pragma once

#include <feature/privacy/services/privacy-feature-service.hxx>
#include <json/value.h>

struct ResponsePrivacyDto
{
  PrivacyView view;
  bool surveillanceActive{true};

  [[nodiscard]] Json::Value toJson() const;
};

struct ResponsePrivacyDirectoryDto
{
  PrivacyDirectory directory;
  bool surveillanceActive{true};

  [[nodiscard]] Json::Value toJson() const;
};
