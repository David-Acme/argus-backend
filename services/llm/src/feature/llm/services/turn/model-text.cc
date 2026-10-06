#include "model-text.hxx"

#include <feature/llm/services/tools/module-command.hxx>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace turn
{

namespace
{
constexpr std::size_t kLongestTitle = 120;
constexpr int kMaxTokens = 64;

struct Wording
{
  std::string_view field;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<Wording, 2> kWordings{{
    {.field = "title",
     .es = "el nombre del elemento, copiado palabra por palabra, sin la fecha ni la hora ni el verbo de la orden",
     .en = "the name of the item, copied word for word, without the date, the time or the verb of the command"},
    {.field = "name",
     .es = "el nombre del elemento, copiado palabra por palabra, sin la fecha ni la hora ni el verbo de la orden",
     .en = "the name of the item, copied word for word, without the date, the time or the verb of the command"},
}};

std::string templateOf(std::string_view field)
{
  std::string out = R"({")";
  out += field;
  out += R"(": ""})";
  return out;
}

std::string schemaOf(const Wording& wording, std::string_view lang)
{
  std::string out = R"({"type": "object", "properties": {")";
  out += wording.field;
  out += R"(": {"type": "string", "description": ")";
  out += lang == "en" ? wording.en : wording.es;
  out += R"("}}})";
  return out;
}

bool copiedFrom(const std::string& value, std::string_view utterance)
{
  std::vector<std::string> heard = module_command::tokensOf(utterance);
  const std::vector<std::string> words = module_command::tokensOf(value);
  if (words.empty())
    return false;
  for (const std::string& word : words) {
    const auto found = std::ranges::find(heard, word);
    if (found == heard.end())
      return false;
    heard.erase(found);
  }
  return true;
}
}

std::optional<std::string> ModelText::extract(const slots::TextRequest& request) const
{
  if (auto ruled = rules_.extract(request))
    return ruled;
  const auto wording = std::ranges::find(kWordings, request.field, &Wording::field);
  if (wording == kWordings.end() || !model_.isLoaded())
    return std::nullopt;
  const auto root = model_.extract({.text = std::string(request.utterance),
                                    .templateJson = templateOf(request.field),
                                    .schemaJson = schemaOf(*wording, request.lang),
                                    .maxTokens = kMaxTokens,
                                    .grammar = true,
                                    .cancel = CancellationToken{}});
  if (!root || !root->isObject() || !(*root)[std::string(request.field)].isString())
    return std::nullopt;
  std::string value = (*root)[std::string(request.field)].asString();
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return std::nullopt;
  value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
  if (value.empty() || value.size() > kLongestTitle || !copiedFrom(value, request.utterance))
    return std::nullopt;
  return slots::capitalized(std::move(value));
}

}
