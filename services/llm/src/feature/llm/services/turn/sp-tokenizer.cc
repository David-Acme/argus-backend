#include "sp-tokenizer.hxx"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>

namespace turn
{

namespace
{
using json = nlohmann::json;

constexpr char kPairSeparator = '\x01';
constexpr float kUnknownScore = -100.0F;

struct PairKey
{
  const std::string& left;
  const std::string& right;
};

std::string pairKey(const PairKey& input)
{
  std::string key = input.left;
  key += kPairSeparator;
  key += input.right;
  return key;
}

std::vector<std::string> codePoints(const std::string& text)
{
  std::vector<std::string> out;
  for (std::size_t at = 0; at < text.size();) {
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t size = 1;
    if ((lead & 0xE0U) == 0xC0U)
      size = 2;
    else if ((lead & 0xF0U) == 0xE0U)
      size = 3;
    else if ((lead & 0xF8U) == 0xF0U)
      size = 4;
    if (at + size > text.size())
      size = 1;
    out.push_back(text.substr(at, size));
    at += size;
  }
  return out;
}

std::string byteToken(unsigned char value)
{
  static const char* digits = "0123456789ABCDEF";
  std::string token = "<0x";
  token += digits[static_cast<unsigned>(value) >> 4U];
  token += digits[value & 0x0FU];
  token += '>';
  return token;
}

bool isSpace(char c)
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string trimmedRight(std::string text)
{
  while (!text.empty() && isSpace(text.back()))
    text.pop_back();
  return text;
}
}

bool SpTokenizer::load(const std::filesystem::path& jsonPath)
{
  std::ifstream file(jsonPath);
  if (!file.is_open())
    return false;
  json root;
  try {
    file >> root;
  }
  catch (const std::exception&) {
    return false;
  }
  try {
    if (root.contains("normalizer") && !root["normalizer"].is_null()) {
      const json& normalizer = root["normalizer"];
      std::function<void(const json&)> apply = [this, &apply](const json& node) {
        const std::string type = node.value("type", "");
        if (type == "Replace") {
          const json& pattern = node.at("pattern");
          if (pattern.contains("String") && pattern["String"].get<std::string>() == " ")
            markerSpaces_ = true;
          else if (pattern.contains("Regex"))
            collapseWhitespace_ = true;
        }
        else if (type == "Strip") {
          stripRight_ = node.value("strip_right", false);
        }
        else if (type == "Sequence") {
          for (const json& step : node.at("normalizers"))
            apply(step);
        }
      };
      apply(normalizer);
    }
    if (root.contains("pre_tokenizer") && !root["pre_tokenizer"].is_null()) {
      const json& pre = root["pre_tokenizer"];
      std::function<void(const json&)> apply = [this, &apply](const json& node) {
        const std::string type = node.value("type", "");
        if (type == "Metaspace")
          marker_ = node.value("replacement", marker_);
        else if (type == "Sequence") {
          const char* key = node.contains("pretokenizers") ? "pretokenizers" : "pre_tokenizers";
          for (const json& step : node.at(key))
            apply(step);
        }
      };
      apply(pre);
    }
    const json& model = root.at("model");
    const std::string type = model.at("type").get<std::string>();
    model_ = type == "BPE" ? SpModel::Bpe : type == "Unigram" ? SpModel::Unigram : SpModel::None;
    if (model_ == SpModel::None)
      return false;
    byteFallback_ = model.value("byte_fallback", false);
    ignoreMerges_ = model.value("ignore_merges", false);
    fuseUnk_ = model.value("fuse_unk", true);
    unk_ = model.contains("unk_id") ? model["unk_id"].get<std::int32_t>() : 0;
    const json& vocab = model.at("vocab");
    if (vocab.is_array()) {
      for (std::size_t at = 0; at < vocab.size(); ++at) {
        const json& entry = vocab[at];
        const std::string token = entry.at(0).get<std::string>();
        vocab_[token] = static_cast<std::int32_t>(at);
        scores_[token] = entry.size() > 1 ? entry.at(1).get<float>() : 0.0F;
      }
    }
    else {
      for (const auto& entry : vocab.items()) {
        if (entry.value().is_number())
          vocab_[entry.key()] = entry.value().get<std::int32_t>();
        else
          vocab_[entry.value().at("token").get<std::string>()] = entry.value().at("id").get<std::int32_t>();
      }
    }
    if (model_ == SpModel::Bpe && model.contains("merges")) {
      const json& merges = model["merges"];
      for (std::size_t rank = 0; rank < merges.size(); ++rank) {
        const json& entry = merges[rank];
        std::string left;
        std::string right;
        if (entry.is_array() && entry.size() >= 2) {
          left = entry.at(0).get<std::string>();
          right = entry.at(1).get<std::string>();
        }
        else if (entry.is_string()) {
          const std::string text = entry.get<std::string>();
          const std::size_t split = text.find(' ');
          if (split == std::string::npos)
            continue;
          left = text.substr(0, split);
          right = text.substr(split + 1);
        }
        else {
          continue;
        }
        merges_.emplace(pairKey({.left = left, .right = right}), static_cast<std::int32_t>(rank));
      }
    }
    if (root.contains("added_tokens")) {
      bool sawCls = false;
      bool sawSep = false;
      for (const json& token : root["added_tokens"]) {
        const std::string content = token.value("content", "");
        const std::int32_t id = token.value("id", 0);
        if (!content.empty())
          added_[content] = id;
        if (content == "<bos>") {
          cls_ = id;
          sawCls = true;
        }
        else if (content == "<eos>") {
          sep_ = id;
          sawSep = true;
        }
        else if (content == "<s>") {
          if (!sawCls)
            cls_ = id;
        }
        else if (content == "</s>") {
          if (!sawSep)
            sep_ = id;
        }
        else if (content == "<mask>") {
          mask_ = id;
          maskToken_ = content;
        }
        else if (content == "<pad>") {
          pad_ = id;
        }
        else if (content == "<unk>") {
          unk_ = id;
        }
      }
    }
    if (!vocab_.empty() && vocab_.contains("<unk>"))
      unk_ = vocab_.at("<unk>");
  }
  catch (const std::exception&) {
    model_ = SpModel::None;
    return false;
  }
  return model_ != SpModel::None && !vocab_.empty();
}

const std::string& SpTokenizer::maskToken() const
{
  return maskToken_;
}

std::optional<std::int32_t> SpTokenizer::addedToken(std::string_view text) const
{
  const auto found = added_.find(std::string(text));
  if (found == added_.end())
    return std::nullopt;
  return found->second;
}

std::string SpTokenizer::normalize(std::string_view text) const{
  std::string out;
  out.reserve(text.size());
  bool lastSpace = false;
  for (const char c : text) {
    if (collapseWhitespace_ && isSpace(c)) {
      if (lastSpace)
        continue;
      out += ' ';
      lastSpace = true;
      continue;
    }
    lastSpace = c == ' ';
    out += c;
  }
  if (stripRight_)
    out = trimmedRight(std::move(out));
  if (markerSpaces_) {
    std::string marked;
    marked.reserve(out.size() + marker_.size());
    for (const char c : out) {
      if (c == ' ')
        marked += marker_;
      else
        marked += c;
    }
    out = std::move(marked);
  }
  return out;
}

std::vector<std::string> SpTokenizer::preTokenize(const std::string& text) const
{
  std::vector<std::string> pieces;
  if (text.empty())
    return pieces;
  std::string marked;
  marked.reserve(text.size() + marker_.size());
  for (const char c : text) {
    if (c == ' ')
      marked += marker_;
    else
      marked += c;
  }
  if (!marked.starts_with(marker_))
    marked.insert(0, marker_);
  std::size_t at = 0;
  while (at < marked.size()) {
    std::size_t next = marked.find(marker_, at + marker_.size());
    if (next == std::string::npos)
      next = marked.size();
    pieces.push_back(marked.substr(at, next - at));
    at = next;
  }
  return pieces;
}

std::vector<std::int32_t> SpTokenizer::bytesOf(const std::string& symbol) const
{
  std::vector<std::int32_t> ids;
  if (!byteFallback_) {
    ids.push_back(unk_);
    return ids;
  }
  for (const unsigned char c : symbol) {
    const auto found = vocab_.find(byteToken(c));
    ids.push_back(found == vocab_.end() ? unk_ : found->second);
  }
  return ids;
}

std::vector<std::int32_t> SpTokenizer::fuse(const std::vector<std::int32_t>& ids) const
{
  if (!fuseUnk_)
    return ids;
  std::vector<std::int32_t> out;
  out.reserve(ids.size());
  for (const std::int32_t id : ids) {
    if (id == unk_ && !out.empty() && out.back() == unk_)
      continue;
    out.push_back(id);
  }
  return out;
}

std::vector<std::int32_t> SpTokenizer::bpe(const std::string& piece) const
{
  std::vector<std::string> symbols = codePoints(piece);
  if (const auto whole = vocab_.find(piece); ignoreMerges_ && whole != vocab_.end())
    return fuse({whole->second});
  while (symbols.size() > 1) {
    std::int32_t bestRank = std::numeric_limits<std::int32_t>::max();
    std::size_t bestAt = symbols.size();
    for (std::size_t at = 0; at + 1 < symbols.size(); ++at) {
      const auto found = merges_.find(pairKey({.left = symbols[at], .right = symbols[at + 1]}));
      if (found != merges_.end() && found->second < bestRank) {
        bestRank = found->second;
        bestAt = at;
      }
    }
    if (bestAt == symbols.size())
      break;
    const std::string merged = symbols[bestAt] + symbols[bestAt + 1];
    std::vector<std::string> next;
    next.reserve(symbols.size());
    for (std::size_t at = 0; at < symbols.size();) {
      if (at + 1 < symbols.size() && symbols[at] == symbols[bestAt] && symbols[at + 1] == symbols[bestAt + 1]) {
        next.push_back(merged);
        at += 2;
        continue;
      }
      next.push_back(symbols[at]);
      ++at;
    }
    symbols = std::move(next);
  }
  std::vector<std::int32_t> ids;
  for (const std::string& symbol : symbols) {
    const auto found = vocab_.find(symbol);
    if (found != vocab_.end()) {
      ids.push_back(found->second);
      continue;
    }
    for (const std::int32_t id : bytesOf(symbol))
      ids.push_back(id);
  }
  return fuse(ids);
}

std::vector<std::int32_t> SpTokenizer::unigram(const std::string& piece) const
{
  const std::vector<std::string> chars = codePoints(piece);
  const std::size_t size = chars.size();
  std::vector<float> best(size + 1, -std::numeric_limits<float>::max());
  std::vector<std::size_t> from(size + 1, 0);
  std::vector<std::int32_t> id(size + 1, unk_);
  best[0] = 0.0F;
  for (std::size_t at = 0; at < size; ++at) {
    if (best[at] == -std::numeric_limits<float>::max())
      continue;
    std::string candidate;
    for (std::size_t end = at; end < size; ++end) {
      candidate += chars[end];
      const auto found = scores_.find(candidate);
      if (found == scores_.end())
        continue;
      const float score = best[at] + found->second;
      if (score > best[end + 1]) {
        best[end + 1] = score;
        from[end + 1] = at;
        id[end + 1] = vocab_.contains(candidate) ? vocab_.at(candidate) : unk_;
      }
    }
    if (best[at + 1] == -std::numeric_limits<float>::max()) {
      const auto found = vocab_.find(chars[at]);
      const float score = best[at] + (found == vocab_.end() ? kUnknownScore : scores_.contains(chars[at]) ? scores_.at(chars[at]) : 0.0F);
      if (score > best[at + 1]) {
        best[at + 1] = score;
        from[at + 1] = at;
        id[at + 1] = found == vocab_.end() ? unk_ : found->second;
      }
    }
  }
  std::vector<std::int32_t> ids;
  for (std::size_t at = size; at > 0; at = from[at]) {
    if (id[at] == unk_ && byteFallback_) {
      const std::string& symbol = chars[at - 1];
      const auto whole = vocab_.find(symbol);
      if (whole == vocab_.end()) {
        const std::vector<std::int32_t> bytes = bytesOf(symbol);
        ids.insert(ids.end(), bytes.rbegin(), bytes.rend());
        continue;
      }
      ids.push_back(whole->second);
      continue;
    }
    ids.push_back(id[at]);
  }
  std::ranges::reverse(ids);
  return fuse(ids);
}

std::vector<std::int32_t> SpTokenizer::encode(std::string_view text, bool addSpecialTokens) const
{
  if (!loaded())
    return {};
  const std::string normalized = normalize(text);
  std::vector<std::int32_t> ids;
  for (const std::string& piece : preTokenize(normalized)) {
    const std::vector<std::int32_t> pieceIds = model_ == SpModel::Bpe ? bpe(piece) : unigram(piece);
    ids.insert(ids.end(), pieceIds.begin(), pieceIds.end());
  }
  if (addSpecialTokens) {
    ids.insert(ids.begin(), cls_);
    ids.push_back(sep_);
  }
  return ids;
}

}
