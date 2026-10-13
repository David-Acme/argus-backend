#pragma once

#include <functional>
#include <json/value.h>
#include <string>
#include <utility>

namespace tls_health
{

struct LoadedTls
{
  std::string certificatePem;
  std::string trustBundlePem;
};

inline constexpr int kWarningDays = 30;

[[nodiscard]] Json::Value report(const LoadedTls& loaded);

[[nodiscard]] std::pair<std::string, std::function<Json::Value()>>
healthExtra(LoadedTls loaded);

}
