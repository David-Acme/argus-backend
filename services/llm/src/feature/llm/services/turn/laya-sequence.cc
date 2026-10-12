#include "laya-sequence.hxx"

#include "sp-tokenizer.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>

namespace turn
{

namespace
{
constexpr std::size_t kOptionTokenCap = 48;
constexpr std::int64_t kMinOptionBudget = 16;
constexpr std::int64_t kMinHead = 8;
constexpr std::int64_t kMinOption = 4;
constexpr int kScientificAtOrBelow = -4;
constexpr int kScientificAbove = 16;
constexpr std::string_view kNoulFalse = "no, the statement does not hold";
constexpr std::string_view kNoulTrue = "yes, the statement holds";
constexpr std::array<char, 16> kHex{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

std::string withoutMask(std::string text, const std::string& mask)
{
  if (mask.empty())
    return text;
  std::size_t at = text.find(mask);
  while (at != std::string::npos) {
    text.replace(at, mask.size(), " ");
    at = text.find(mask, at + 1);
  }
  return text;
}

void appendQuoted(std::string& out, std::string_view text)
{
  out += '"';
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (byte < 0x20U) {
          out += "\\u00";
          out += kHex[byte >> 4U];
          out += kHex[byte & 0x0FU];
        }
        else {
          out += c;
        }
    }
  }
  out += '"';
}

std::string pythonFloat(double value)
{
  if (std::isnan(value))
    return "NaN";
  if (std::isinf(value))
    return value > 0 ? "Infinity" : "-Infinity";
  std::array<char, 64> buffer{};
  const std::to_chars_result written =
      std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::scientific);
  if (written.ec != std::errc())
    return "0.0";
  const std::string_view text(buffer.data(), static_cast<std::size_t>(written.ptr - buffer.data()));
  const std::size_t e = text.find('e');
  std::string_view mantissa = text.substr(0, e);
  const int exponent = std::atoi(std::string(text.substr(e + 1)).c_str());
  std::string sign;
  if (!mantissa.empty() && mantissa.front() == '-') {
    sign = "-";
    mantissa.remove_prefix(1);
  }
  std::string digits;
  for (const char c : mantissa)
    if (c != '.')
      digits += c;
  const int point = exponent + 1;
  if (point <= kScientificAtOrBelow || point > kScientificAbove) {
    std::string out = sign;
    out += digits.front();
    if (digits.size() > 1) {
      out += '.';
      out += digits.substr(1);
    }
    out += exponent < 0 ? "e-" : "e+";
    const int magnitude = std::abs(exponent);
    if (magnitude < 10)
      out += '0';
    out += std::to_string(magnitude);
    return out;
  }
  if (point <= 0)
    return sign + "0." + std::string(static_cast<std::size_t>(-point), '0') + digits;
  const auto whole = static_cast<std::size_t>(point);
  if (whole < digits.size())
    return sign + digits.substr(0, whole) + "." + digits.substr(whole);
  return sign + digits + std::string(whole - digits.size(), '0') + ".0";
}

void appendPython(std::string& out, const nlohmann::ordered_json& value)
{
  switch (value.type()) {
    case nlohmann::ordered_json::value_t::null:
      out += "null";
      return;
    case nlohmann::ordered_json::value_t::boolean:
      out += value.get<bool>() ? "true" : "false";
      return;
    case nlohmann::ordered_json::value_t::number_integer:
      out += std::to_string(value.get<std::int64_t>());
      return;
    case nlohmann::ordered_json::value_t::number_unsigned:
      out += std::to_string(value.get<std::uint64_t>());
      return;
    case nlohmann::ordered_json::value_t::number_float:
      out += pythonFloat(value.get<double>());
      return;
    case nlohmann::ordered_json::value_t::string:
      appendQuoted(out, value.get_ref<const std::string&>());
      return;
    case nlohmann::ordered_json::value_t::array: {
      out += '[';
      bool first = true;
      for (const nlohmann::ordered_json& item : value) {
        if (!first)
          out += ", ";
        first = false;
        appendPython(out, item);
      }
      out += ']';
      return;
    }
    case nlohmann::ordered_json::value_t::object: {
      out += '{';
      bool first = true;
      for (const auto& [key, item] : value.items()) {
        if (!first)
          out += ", ";
        first = false;
        appendQuoted(out, key);
        out += ": ";
        appendPython(out, item);
      }
      out += '}';
      return;
    }
    default:
      out += "null";
  }
}

std::string criterionText(const nlohmann::ordered_json& value)
{
  return value.is_string() ? value.get<std::string>() : layaPythonJson(value);
}

bool undescribed(const nlohmann::ordered_json& value)
{
  return value.is_null() || (value.is_string() && value.get_ref<const std::string&>().empty());
}

std::string stripped(std::string_view text)
{
  constexpr std::string_view kSpace = " \t\n\r\f\v";
  const std::size_t first = text.find_first_not_of(kSpace);
  if (first == std::string_view::npos)
    return {};
  return std::string(text.substr(first, text.find_last_not_of(kSpace) - first + 1));
}

std::string lowered(std::string text)
{
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool instructionsAccepted(const nlohmann::ordered_json& instructions)
{
  if (instructions.is_string())
    return !stripped(instructions.get_ref<const std::string&>()).empty();
  if (instructions.is_object() || instructions.is_array())
    return !instructions.empty();
  return instructions.is_number() || instructions.is_boolean();
}

std::optional<LayaQuestion> choiceQuestion(LayaQuestion question, const nlohmann::ordered_json& criteria)
{
  if (criteria.is_array()) {
    for (const nlohmann::ordered_json& label : criteria) {
      if (!label.is_string())
        return std::nullopt;
      const std::string& text = label.get_ref<const std::string&>();
      if (std::ranges::find(question.labels, text) != question.labels.end())
        return std::nullopt;
      question.labels.push_back(text);
      question.options.push_back(text);
    }
  }
  else if (criteria.is_object()) {
    for (const auto& [label, description] : criteria.items()) {
      question.labels.push_back(label);
      question.options.push_back(undescribed(description) ? label : label + ": " + criterionText(description));
    }
  }
  else {
    return std::nullopt;
  }
  if (question.options.empty())
    return std::nullopt;
  return question;
}

std::optional<LayaQuestion> scoreQuestion(LayaQuestion question, const nlohmann::ordered_json& criteria)
{
  if (!criteria.is_array() || criteria.empty())
    return std::nullopt;
  std::size_t level = 0;
  for (const nlohmann::ordered_json& entry : criteria) {
    if (entry.is_null())
      return std::nullopt;
    question.labels.push_back(std::to_string(level));
    question.options.push_back("level " + std::to_string(level) + ": " + criterionText(entry));
    ++level;
  }
  return question;
}

std::optional<LayaQuestion> noulQuestion(LayaQuestion question, const nlohmann::ordered_json& definition)
{
  std::string falseLabel = "false";
  std::string trueLabel = "true";
  if (definition.contains("labels")) {
    const nlohmann::ordered_json& labels = definition["labels"];
    if (!labels.is_object() || labels.size() != 2 || !labels.contains("false") || !labels.contains("true") ||
        !labels["false"].is_string() || !labels["true"].is_string())
      return std::nullopt;
    falseLabel = stripped(labels["false"].get_ref<const std::string&>());
    trueLabel = stripped(labels["true"].get_ref<const std::string&>());
    if (falseLabel.empty() || trueLabel.empty() || falseLabel == trueLabel)
      return std::nullopt;
  }
  nlohmann::ordered_json falseText;
  nlohmann::ordered_json trueText;
  if (definition.contains("criteria") && !definition["criteria"].is_null()) {
    const nlohmann::ordered_json& criteria = definition["criteria"];
    if (!criteria.is_object())
      return std::nullopt;
    for (const auto& [key, value] : criteria.items()) {
      const std::string name = lowered(key);
      if (name == "false")
        falseText = value;
      else if (name == "true")
        trueText = value;
      else
        return std::nullopt;
    }
  }
  question.labels = {"false", "true"};
  question.options = {falseLabel + ": " + (undescribed(falseText) ? std::string(kNoulFalse) : criterionText(falseText)),
                      trueLabel + ": " + (undescribed(trueText) ? std::string(kNoulTrue) : criterionText(trueText))};
  return question;
}
}

std::string layaPythonJson(const nlohmann::ordered_json& value)
{
  std::string out;
  appendPython(out, value);
  return out;
}

std::optional<LayaQuestion> layaQuestionFromJson(const nlohmann::ordered_json& definition)
{
  if (!definition.is_object() || !definition.contains("type") || !definition["type"].is_string() || !definition.contains("instructions"))
    return std::nullopt;
  const nlohmann::ordered_json& instructions = definition["instructions"];
  if (!instructionsAccepted(instructions))
    return std::nullopt;
  LayaQuestion question;
  question.instructions = instructions.is_string() ? instructions.get<std::string>() : layaPythonJson(instructions);
  const std::string& type = definition["type"].get_ref<const std::string&>();
  static const nlohmann::ordered_json kAbsent;
  const nlohmann::ordered_json& criteria = definition.contains("criteria") ? definition["criteria"] : kAbsent;
  if (type != "noul" && definition.contains("labels"))
    return std::nullopt;
  if (type == "choice") {
    question.kind = QuestionKind::Choice;
    return choiceQuestion(std::move(question), criteria);
  }
  if (type == "score") {
    question.kind = QuestionKind::Score;
    return scoreQuestion(std::move(question), criteria);
  }
  if (type == "noul") {
    question.kind = QuestionKind::Noul;
    return noulQuestion(std::move(question), definition);
  }
  return std::nullopt;
}

LayaSequence layaBuildSequence(const LayaSequenceInput& input, const LayaQuestion& question)
{
  LayaSequence sequence;
  const SpTokenizer& tokenizer = input.tokenizer;
  const std::string& mask = tokenizer.maskToken();
  std::vector<std::vector<std::int32_t>> optionTokens;
  optionTokens.reserve(question.options.size());
  for (const std::string& option : question.options) {
    std::vector<std::int32_t> ids = tokenizer.encode(" " + withoutMask(option, mask), false);
    if (ids.size() > kOptionTokenCap)
      ids.resize(kOptionTokenCap);
    std::vector<std::int32_t> withMask;
    withMask.reserve(ids.size() + 1);
    withMask.push_back(tokenizer.maskId());
    withMask.insert(withMask.end(), ids.begin(), ids.end());
    optionTokens.push_back(std::move(withMask));
  }
  const auto optionTotal = [&optionTokens] {
    std::int64_t total = 0;
    for (const std::vector<std::int32_t>& option : optionTokens)
      total += static_cast<std::int64_t>(option.size());
    return total;
  };
  const auto headMaxLen = static_cast<std::int64_t>(input.headMaxLen);
  std::int64_t budget = headMaxLen - optionTotal();
  if (budget < kMinOptionBudget) {
    const auto count = std::max<std::int64_t>(1, static_cast<std::int64_t>(optionTokens.size()));
    const std::int64_t spare = headMaxLen - kMinOptionBudget;
    const std::int64_t floored = spare >= 0 ? spare / count : -((-spare + count - 1) / count);
    const auto per = static_cast<std::size_t>(std::max(kMinOption, floored));
    for (std::vector<std::int32_t>& option : optionTokens)
      if (option.size() > per)
        option.resize(per);
    budget = headMaxLen - optionTotal();
  }
  const std::string_view kindName =
      question.kind == QuestionKind::Score ? "score" : question.kind == QuestionKind::Noul ? "noul" : "choice";
  std::vector<std::int32_t> head =
      tokenizer.encode(std::string(kindName) + " question: " + withoutMask(question.instructions, mask), false);
  const auto headCap = static_cast<std::size_t>(std::max(kMinHead, budget));
  if (head.size() > headCap)
    head.resize(headCap);
  std::vector<std::int32_t> ids;
  ids.reserve(head.size() + static_cast<std::size_t>(optionTotal()) + 2);
  ids.push_back(tokenizer.clsId());
  ids.insert(ids.end(), head.begin(), head.end());
  ids.push_back(tokenizer.sepId());
  for (const std::vector<std::int32_t>& option : optionTokens) {
    sequence.markers.push_back(ids.size());
    ids.insert(ids.end(), option.begin(), option.end());
  }
  ids.push_back(tokenizer.sepId());
  const std::int64_t room =
      std::max<std::int64_t>(0, static_cast<std::int64_t>(input.maxLen) - static_cast<std::int64_t>(ids.size()) - 1);
  std::vector<std::int32_t> state = tokenizer.encode(withoutMask(std::string(input.state), mask), false);
  if (std::cmp_greater(state.size(), room))
    state.resize(static_cast<std::size_t>(room));
  ids.insert(ids.end(), state.begin(), state.end());
  ids.push_back(tokenizer.sepId());
  const auto maxLen = static_cast<std::size_t>(std::max(0, input.maxLen));
  if (ids.size() > maxLen)
    ids.resize(maxLen);
  std::erase_if(sequence.markers, [maxLen](std::size_t marker) { return marker >= maxLen; });
  sequence.ids = std::move(ids);
  return sequence;
}

}
