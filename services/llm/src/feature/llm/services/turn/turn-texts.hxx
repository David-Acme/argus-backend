#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

struct DayQuestion
{
  int64_t first{0};
  int64_t second{0};
  int64_t now{0};
  std::string_view lang;
};

[[nodiscard]] std::string dayQuestion(const DayQuestion& question);

struct ProjectQuestion
{
  const std::vector<std::string>& options;
  std::string_view lang;
};

[[nodiscard]] std::string projectQuestion(const ProjectQuestion& question);

[[nodiscard]] std::string newProjectQuestion(std::string_view lang, bool noneYet);

struct CreateProject
{
  std::string_view name;
  std::string_view lang;
};

[[nodiscard]] std::string createProjectQuestion(const CreateProject& project);

[[nodiscard]] std::string cannotCreateProject(std::string_view lang);

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
