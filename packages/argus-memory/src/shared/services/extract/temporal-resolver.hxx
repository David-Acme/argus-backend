#pragma once

#include <shared/services/extract/extract-contracts.hxx>
#include <string>
#include <string_view>

struct TemporalResolveInput
{
  std::string_view lang;
  std::string normalized;
};

class TemporalResolver
{
public:
  extract::TemporalValue
  resolve(const TemporalResolveInput& input) const;

  static std::string normalize(std::string_view text);

private:
  int weekdayFor(std::string_view lang, std::string_view token) const;
  int hourFor(std::string_view token) const;
};
