#include "relay-command.hxx"

#include <text/name-match.hxx>

#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace relay_command
{

namespace
{

constexpr std::string_view kTool = "message.relay";
constexpr std::string_view kWakeWord = "argus";
constexpr std::string_view kNegation = "no";
constexpr std::size_t kMaxRecipientWords = 3;

struct Family
{
  std::vector<std::string_view> lead;
  std::vector<std::string_view> joint;
  std::string_view tail;
  bool reply;
};

const std::vector<Family>& families()
{
  static const std::vector<Family> kFamilies{
      {.lead = {"llama", "a"}, .joint = {"y", "dile"}, .tail = "que", .reply = false},
      {.lead = {"dile", "a"}, .joint = {"que"}, .tail = {}, .reply = false},
      {.lead = {"avisale", "a"}, .joint = {"que"}, .tail = {}, .reply = false},
      {.lead = {"avisa", "a"}, .joint = {"que"}, .tail = {}, .reply = false},
      {.lead = {"mandale", "a"}, .joint = {"que"}, .tail = {}, .reply = false},
      {.lead = {"cuentale", "a"}, .joint = {"que"}, .tail = {}, .reply = false},
      {.lead = {"let"}, .joint = {"know", "that"}, .tail = {}, .reply = false},
      {.lead = {"tell"}, .joint = {"that"}, .tail = {}, .reply = false},
      {.lead = {"dile"}, .joint = {"que"}, .tail = {}, .reply = true},
  };
  return kFamilies;
}

struct Word
{
  std::string text;
  std::size_t token{0};
};

std::vector<std::string> tokensOf(std::string_view utterance)
{
  std::istringstream in{std::string(utterance)};
  std::vector<std::string> tokens;
  std::string token;
  while (in >> token)
    tokens.push_back(std::move(token));
  return tokens;
}

std::vector<Word> wordsOf(const std::vector<std::string>& tokens)
{
  std::vector<Word> words;
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    std::istringstream in{text_norm::folded(tokens[index])};
    std::string word;
    while (in >> word)
      words.push_back({.text = word, .token = index});
  }
  return words;
}

bool startsWith(const std::vector<Word>& words, std::size_t at, const std::vector<std::string_view>& sequence)
{
  if (at + sequence.size() > words.size())
    return false;
  for (std::size_t offset = 0; offset < sequence.size(); ++offset)
    if (words[at + offset].text != sequence[offset])
      return false;
  return true;
}

std::optional<std::size_t> jointAt(const std::vector<Word>& words, const Family& family)
{
  const std::size_t span = family.reply ? 0 : kMaxRecipientWords;
  for (std::size_t at = family.lead.size(); at <= family.lead.size() + span; ++at)
    if (startsWith(words, at, family.joint))
      return at;
  return std::nullopt;
}

std::string recipientOf(const std::vector<Word>& words, const Family& family, std::size_t joint)
{
  std::string recipient;
  for (std::size_t at = family.lead.size(); at < joint; ++at) {
    if (!recipient.empty())
      recipient.push_back(' ');
    recipient += words[at].text;
  }
  return recipient;
}

std::string messageOf(const std::vector<std::string>& tokens, const std::vector<Word>& words, std::size_t next)
{
  if (next >= words.size())
    return {};
  std::string message;
  for (std::size_t index = words[next].token; index < tokens.size(); ++index) {
    if (!message.empty())
      message.push_back(' ');
    message += tokens[index];
  }
  return message;
}

std::optional<Command> matchFamily(const Family& family, const std::vector<Word>& words,
                                   const std::vector<std::string>& tokens)
{
  if (!startsWith(words, 0, family.lead))
    return std::nullopt;
  const auto joint = jointAt(words, family);
  if (!joint)
    return std::nullopt;
  const std::string recipient = recipientOf(words, family, *joint);
  std::size_t next = *joint + family.joint.size();
  if (!family.tail.empty() && next < words.size() && words[next].text == family.tail)
    ++next;
  const std::string message = messageOf(tokens, words, next);

  Command command;
  command.tool = std::string(kTool);
  command.arguments["recipient"] = recipient;
  command.arguments["message"] = message;
  command.arguments["reply"] = family.reply;
  command.reply = family.reply;
  if (recipient.empty())
    command.fill.emplace_back("recipient");
  if (message.empty())
    command.fill.emplace_back("message");
  return command;
}

}

std::optional<Command> commandFor(std::string_view utterance)
{
  const auto tokens = tokensOf(utterance);
  auto words = wordsOf(tokens);
  if (!words.empty() && words.front().text == kWakeWord)
    words.erase(words.begin());
  if (words.empty() || words.front().text == kNegation)
    return std::nullopt;
  for (const auto& family : families())
    if (auto command = matchFamily(family, words, tokens))
      return command;
  return std::nullopt;
}

}
