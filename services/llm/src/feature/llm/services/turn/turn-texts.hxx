#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace turn_texts
{

struct Fact
{
  std::string_view tool;
  std::string_view result;
};

[[nodiscard]] std::string done(std::string_view lang, const Fact& fact);

[[nodiscard]] std::string refused(std::string_view lang, const Fact& fact);

[[nodiscard]] std::string preview(std::string_view lang, std::string_view result);

[[nodiscard]] std::string offer(std::string_view lang, std::string_view result);

[[nodiscard]] std::string declined(std::string_view lang);

[[nodiscard]] std::string unactionable(std::string_view lang);

[[nodiscard]] std::string misunderstood(std::string_view lang);

struct Details
{
  std::string title{};
  std::string when{};
  std::string module{};
};

struct ConfirmQuestion
{
  std::string_view tool;
  std::string_view lang;
  Details details{};
};

[[nodiscard]] std::string confirmQuestion(const ConfirmQuestion& question);

struct ChooseQuestion
{
  std::string_view first;
  std::string_view second;
  std::string_view lang;
};

[[nodiscard]] std::string chooseQuestion(const ChooseQuestion& question);

struct WhenInput
{
  std::string_view iso;
  int64_t now{0};
  std::string_view lang;
};

[[nodiscard]] std::string spokenWhen(const WhenInput& input);

struct SlotQuestion
{
  std::string_view tool;
  std::string_view slot;
  std::string_view lang;
};

[[nodiscard]] std::string slotQuestion(const SlotQuestion& question);

}
