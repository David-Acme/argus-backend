#include "laya-sequence.hxx"

#include "sp-tokenizer.hxx"

#include <json/writer.h>

#include <algorithm>
#include <string>
#include <utility>

namespace turn
{

namespace
{
constexpr std::size_t kOptionTokenCap = 48;
constexpr int kMinOptionBudget = 16;
constexpr int kMinHead = 8;
constexpr std::size_t kMinOption = 4;

std::string withoutMask(std::string text, const std::string& mask)
{
  std::size_t at = text.find(mask);
  while (at != std::string::npos) {
    text.replace(at, mask.size(), " ");
    at = text.find(mask, at + 1);
  }
  return text;
}

std::string compact(const Json::Value& value)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}
}

std::string layaCriterionText(const Json::Value& value)
{
  if (value.isString())
    return value.asString();
  if (value.isNull())
    return {};
  if (value.isBool())
    return value.asBool() ? "true" : "false";
  if (value.isInt64() || value.isUInt64())
    return std::to_string(value.asInt64());
  return compact(value);
}

std::optional<LayaQuestion> layaQuestionFromJson(const Json::Value& definition)
{
  if (!definition.isObject())
    return std::nullopt;
  LayaQuestion question;
  const std::string type = definition.get("type", "choice").asString();
  if (type == "choice")
    question.kind = QuestionKind::Choice;
  else if (type == "score")
    question.kind = QuestionKind::Score;
  else if (type == "noul")
    question.kind = QuestionKind::Noul;
  else
    return std::nullopt;
  const Json::Value& instructions = definition["instructions"];
  question.instructions = instructions.isString() ? instructions.asString() : compact(instructions);
  const Json::Value& criteria = definition["criteria"];
  if (criteria.isArray()) {
    for (const Json::Value& entry : criteria) {
      if (question.kind == QuestionKind::Score)
        question.criteria.emplace_back(std::string(), layaCriterionText(entry));
      else
        question.criteria.emplace_back(entry.isString() ? entry.asString() : compact(entry), std::string());
    }
  }
  else if (criteria.isObject()) {
    for (const std::string& key : criteria.getMemberNames()) {
      const Json::Value& entry = criteria[key];
      const std::string text = entry.isNull() || (entry.isString() && entry.asString().empty()) ? std::string() : layaCriterionText(entry);
      question.criteria.emplace_back(key, text);
    }
  }
  if (definition.isMember("labels") && definition["labels"].isObject()) {
    question.falseLabel = definition["labels"].get("false", "false").asString();
    question.trueLabel = definition["labels"].get("true", "true").asString();
  }
  return question;
}

std::vector<std::string> layaOptions(const LayaQuestion& question)
{
  std::vector<std::string> options;
  if (question.kind == QuestionKind::Choice) {
    for (const auto& [label, description] : question.criteria) {
      std::string option = label;
      if (!description.empty()) {
        option += ": ";
        option += description;
      }
      options.push_back(std::move(option));
    }
    return options;
  }
  if (question.kind == QuestionKind::Score) {
    std::size_t index = 0;
    for (const auto& entry : question.criteria)
      options.push_back("level " + std::to_string(index++) + ": " + entry.second);
    return options;
  }
  const std::string falseText = question.criteria.empty() || question.criteria.front().second.empty()
                                    ? "no, the statement does not hold"
                                    : question.criteria.front().second;
  const std::string trueText = question.criteria.size() < 2 || question.criteria[1].second.empty()
                                   ? "yes, the statement holds"
                                   : question.criteria[1].second;
  options.push_back(question.falseLabel + ": " + falseText);
  options.push_back(question.trueLabel + ": " + trueText);
  return options;
}

LayaSequence layaBuildSequence(const LayaSequenceInput& input, const LayaQuestion& question)
{
  LayaSequence sequence;
  const SpTokenizer& tokenizer = input.tokenizer;
  const std::string& mask = tokenizer.maskToken();
  std::vector<std::vector<std::int32_t>> optionTokens;
  for (const std::string& option : layaOptions(question)) {
    std::vector<std::int32_t> ids = tokenizer.encode(withoutMask(" " + option, mask), false);
    if (ids.size() > kOptionTokenCap)
      ids.resize(kOptionTokenCap);
    std::vector<std::int32_t> withMask;
    withMask.reserve(ids.size() + 1);
    withMask.push_back(tokenizer.maskId());
    withMask.insert(withMask.end(), ids.begin(), ids.end());
    optionTokens.push_back(std::move(withMask));
  }
  std::int64_t total = 0;
  for (const std::vector<std::int32_t>& option : optionTokens)
    total += static_cast<std::int64_t>(option.size());
  std::int64_t budget = static_cast<std::int64_t>(input.headMaxLen) - total;
  if (budget < kMinOptionBudget && !optionTokens.empty()) {
    const auto per = std::max(kMinOption, (static_cast<std::size_t>(input.headMaxLen) - kMinOptionBudget) / optionTokens.size());
    for (std::vector<std::int32_t>& option : optionTokens)
      if (option.size() > per)
        option.resize(per);
    total = 0;
    for (const std::vector<std::int32_t>& option : optionTokens)
      total += static_cast<std::int64_t>(option.size());
    budget = static_cast<std::int64_t>(input.headMaxLen) - total;
  }
  const std::string kindName = question.kind == QuestionKind::Score ? "score" : question.kind == QuestionKind::Noul ? "noul" : "choice";
  std::vector<std::int32_t> head =
      tokenizer.encode(kindName + " question: " + withoutMask(question.instructions, mask), false);
  const std::size_t headCap = static_cast<std::size_t>(std::max<std::int64_t>(kMinHead, budget));
  if (head.size() > headCap)
    head.resize(headCap);
  std::vector<std::int32_t> ids;
  ids.reserve(head.size() + static_cast<std::size_t>(total) + 2);
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
  if (ids.size() > static_cast<std::size_t>(input.maxLen))
    ids.resize(static_cast<std::size_t>(input.maxLen));
  std::erase_if(sequence.markers, [&ids](std::size_t marker) { return marker >= ids.size(); });
  sequence.ids = std::move(ids);
  return sequence;
}

}
