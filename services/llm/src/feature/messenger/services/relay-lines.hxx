#pragma once

#include <feature/messenger/vocabulary/relay-outcome.hxx>

#include <string>
#include <string_view>
#include <vector>

namespace relay_lines
{

struct SenderLine
{
  std::string_view sender;
  std::string_view message;
  std::string_view lang;
};

struct VisitorLine
{
  std::string_view camera;
  std::string_view need;
  std::string_view lang;
};

struct ConsentLine
{
  std::string_view recipient;
  std::string_view message;
  std::string_view lang;
};

[[nodiscard]] std::string outcome(RelayOutcome outcome, std::string_view lang);

[[nodiscard]] std::string consentQuestion(const ConsentLine& line);

[[nodiscard]] std::string consentExpired(std::string_view lang);

[[nodiscard]] std::string recipientQuestion(const std::vector<std::string>& candidates, std::string_view lang);

[[nodiscard]] std::string messageFromSender(const SenderLine& line);

[[nodiscard]] std::string visitorAsk(std::string_view lang);

[[nodiscard]] std::string visitorDone(std::string_view lang);

[[nodiscard]] std::string visitorTitle(std::string_view camera, std::string_view lang);

[[nodiscard]] std::string visitorMessage(const VisitorLine& line);

}
