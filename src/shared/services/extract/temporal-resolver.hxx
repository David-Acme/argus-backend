#pragma once

#include <shared/services/extract/extract-contracts.hxx>
#include <string>
#include <string_view>

class TemporalResolver
{
public:
  void resolve(std::string_view lang, const std::string& normalized,
               extract::TemporalValue& out) const;

  static std::string normalize(std::string_view text);

private:
  int weekdayFor(std::string_view lang, std::string_view token) const;
  int hourFor(std::string_view token) const;
};
