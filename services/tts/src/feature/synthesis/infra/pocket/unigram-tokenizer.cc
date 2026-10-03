#include "unigram-tokenizer.hxx"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>

namespace
{
constexpr std::string_view kMetaspace = "\xE2\x96\x81";
constexpr double kUnknownPenalty = 10.0;

std::size_t codepointLength(unsigned char lead)
{
  if (lead < 0x80U)
    return 1;
  if ((lead & 0xE0U) == 0xC0U)
    return 2;
  if ((lead & 0xF0U) == 0xE0U)
    return 3;
  if ((lead & 0xF8U) == 0xF0U)
    return 4;
  return 1;
}

std::optional<std::size_t> byteToken(std::string_view piece)
{
  if (piece.size() != 6 || !piece.starts_with("<0x") || !piece.ends_with('>'))
    return std::nullopt;
  std::size_t value = 0;
  for (const char digit : piece.substr(3, 2)) {
    value *= 16;
    if (digit >= '0' && digit <= '9')
      value += static_cast<std::size_t>(digit - '0');
    else if (digit >= 'A' && digit <= 'F')
      value += static_cast<std::size_t>(digit - 'A' + 10);
    else
      return std::nullopt;
  }
  return value;
}

void requireType(const nlohmann::json& node, std::string_view type)
{
  if (!node.is_object() || node.value("type", std::string()) != type)
    throw std::runtime_error("Pocket tokenizer layout is not supported: expected " + std::string(type));
}
}

UnigramTokenizer UnigramTokenizer::load(const std::filesystem::path& path)
{
  std::ifstream stream(path);
  if (!stream)
    throw std::runtime_error("Pocket tokenizer not found: " + path.string());
  const auto json = nlohmann::json::parse(stream);
  const auto& model = json.at("model");
  requireType(model, "Unigram");
  requireType(json.at("pre_tokenizer"), "Metaspace");
  const auto& normalizer = json.at("normalizer");
  if (normalizer.value("type", std::string()) == "Sequence") {
    const auto& steps = normalizer.at("normalizers");
    if (steps.size() != 1)
      throw std::runtime_error("Pocket tokenizer normalizer is not supported");
    requireType(steps.front(), "Prepend");
  }
  else
    requireType(normalizer, "Prepend");
  if (!model.value("byte_fallback", false))
    throw std::runtime_error("Pocket tokenizer needs byte fallback");

  const auto added = json.value("added_tokens", nlohmann::json::array());
  std::vector<std::string> special;
  special.reserve(added.size());
  for (const auto& token : added) {
    if (token.value("special", false))
      special.push_back(token.at("content").get<std::string>());
  }

  UnigramTokenizer tokenizer;
  tokenizer.unknownId_ = model.at("unk_id").get<std::int64_t>();
  tokenizer.byteIds_.fill(-1);
  const auto& vocab = model.at("vocab");
  tokenizer.vocabularySize_ = vocab.size();
  auto minimum = std::numeric_limits<double>::max();
  std::int64_t id = 0;
  for (const auto& entry : vocab) {
    auto piece = entry.at(0).get<std::string>();
    const auto score = entry.at(1).get<double>();
    minimum = std::min(minimum, score);
    if (const auto byte = byteToken(piece); byte.has_value())
      tokenizer.byteIds_.at(*byte) = id;
    else if (std::ranges::find(special, piece) == special.end() && !piece.empty()) {
      tokenizer.maxPieceBytes_ = std::max(tokenizer.maxPieceBytes_, piece.size());
      tokenizer.pieces_.emplace(std::move(piece), Piece{.id = id, .score = score});
    }
    ++id;
  }
  tokenizer.unknownScore_ = minimum - kUnknownPenalty;
  return tokenizer;
}

std::vector<std::int64_t> UnigramTokenizer::encode(std::string_view text) const
{
  std::vector<std::int64_t> ids;
  if (text.empty())
    return ids;
  std::string normalized(kMetaspace);
  normalized.reserve(text.size() * 2);
  for (const char value : text) {
    if (value == ' ')
      normalized += kMetaspace;
    else
      normalized += value;
  }
  const std::string_view view(normalized);
  std::size_t start = 0;
  while (start < view.size()) {
    auto next = view.find(kMetaspace, start + kMetaspace.size());
    if (next == std::string_view::npos)
      next = view.size();
    encodeWord(view.substr(start, next - start), ids);
    start = next;
  }
  return ids;
}

void UnigramTokenizer::encodeWord(std::string_view word, std::vector<std::int64_t>& ids) const
{
  struct Node
  {
    double score{-std::numeric_limits<double>::infinity()};
    std::size_t start{0};
    std::int64_t id{-1};
  };
  std::vector<Node> best(word.size() + 1);
  best.front().score = 0;
  std::size_t begin = 0;
  while (begin < word.size()) {
    const auto step = std::min(codepointLength(static_cast<unsigned char>(word[begin])), word.size() - begin);
    if (best[begin].score != -std::numeric_limits<double>::infinity()) {
      bool single = false;
      auto end = begin + step;
      while (end <= word.size() && end - begin <= maxPieceBytes_) {
        if (const auto found = pieces_.find(word.substr(begin, end - begin)); found != pieces_.end()) {
          const auto candidate = best[begin].score + found->second.score;
          if (candidate > best[end].score)
            best[end] = {.score = candidate, .start = begin, .id = found->second.id};
          if (end == begin + step)
            single = true;
        }
        if (end == word.size())
          break;
        end += std::min(codepointLength(static_cast<unsigned char>(word[end])), word.size() - end);
      }
      if (!single) {
        const auto candidate = best[begin].score + unknownScore_;
        if (candidate > best[begin + step].score)
          best[begin + step] = {.score = candidate, .start = begin, .id = -1};
      }
    }
    begin += step;
  }
  std::vector<Node> path;
  path.reserve(word.size());
  for (auto position = word.size(); position > 0; position = best[position].start)
    path.push_back({.score = 0, .start = position, .id = best[position].id});
  std::size_t cursor = 0;
  for (const auto& node : std::ranges::reverse_view(path)) {
    const auto end = node.start;
    if (node.id >= 0) {
      ids.push_back(node.id);
      cursor = end;
      continue;
    }
    for (const char byte : word.substr(cursor, end - cursor)) {
      const auto byteId = byteIds_.at(static_cast<unsigned char>(byte));
      ids.push_back(byteId >= 0 ? byteId : unknownId_);
    }
    cursor = end;
  }
}
