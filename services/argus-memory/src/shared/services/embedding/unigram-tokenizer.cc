#include "unigram-tokenizer.hxx"

#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using nlohmann::json;

bool UnigramTokenizer::load(const std::string& jsonPath)
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

  const auto& model = root.at("model");
  if (model.at("type") != "Unigram")
    return false;

  pieces_.clear();
  trie_.clear();
  trie_.push_back({});

  const auto& vocab = model.at("vocab");
  pieces_.reserve(vocab.size());
  for (size_t idx = 0; idx < vocab.size(); ++idx) {
    const auto& entry = vocab[idx];
    int32_t id = static_cast<int32_t>(idx);
    std::string token;
    float score = 0.0F;
    if (entry.is_array()) {
      token = entry.at(0).get<std::string>();
      if (entry.size() > 1 && entry.at(1).is_number())
        score = entry.at(1).get<float>();
    }
    else {
      id = entry.at("id").get<int32_t>();
      token = entry.at("token").get<std::string>();
      score = entry.at("score").get<float>();
    }
    if (static_cast<size_t>(id) >= pieces_.size())
      pieces_.resize(static_cast<size_t>(id) + 1);
    pieces_[static_cast<size_t>(id)] = {.id = id, .score = score};

    int32_t node = 0;
    for (unsigned char c : token) {
      auto it = trie_[static_cast<size_t>(node)].next.find(static_cast<char>(c));
      if (it == trie_[static_cast<size_t>(node)].next.end()) {
        trie_[static_cast<size_t>(node)].next[static_cast<char>(c)] =
            static_cast<int32_t>(trie_.size());
        trie_.push_back({});
      }
      node = trie_[static_cast<size_t>(node)].next[static_cast<char>(c)];
    }
    trie_[static_cast<size_t>(node)].piece = id;
  }

  if (model.contains("unk_id"))
    unk_ = model.at("unk_id").get<int32_t>();

  if (root.contains("added_tokens")) {
    for (const auto& t : root.at("added_tokens")) {
      const std::string content = t.at("content").get<std::string>();
      const int32_t id = t.at("id").get<int32_t>();
      if (content == "<s>")
        bos_ = id;
      else if (content == "</s>")
        eos_ = id;
      else if (content == "<pad>")
        pad_ = id;
    }
  }

  return !pieces_.empty();
}

std::vector<int32_t> UnigramTokenizer::encode(const std::string& text,
                                              int maxLen) const
{
  std::string norm;
  norm.reserve(text.size() + 1);
  bool lastSpace = false;
  for (unsigned char c : text) {
    if (c == ' ') {
      if (!lastSpace)
        norm += ' ';
      lastSpace = true;
    }
    else {
      norm += static_cast<char>(c);
      lastSpace = false;
    }
  }
  if (norm.empty())
    norm = " ";

  std::string meta;
  meta.reserve(norm.size() + 1);
  meta += "\xe2\x96\x81";
  for (char c : norm) {
    if (c == ' ')
      meta += "\xe2\x96\x81";
    else
      meta += c;
  }

  const size_t n = meta.size();
  std::vector<float> best(n + 1, -1e30F);
  std::vector<int32_t> prevPiece(n + 1, -1);
  std::vector<int32_t> prevPos(n + 1, -1);
  best[0] = 0.0F;

  for (size_t i = 0; i < n; ++i) {
    if (best[i] <= -1e29F)
      continue;
    int32_t node = 0;
    size_t len = 0;
    int32_t bestPiece = -1;
    size_t bestLen = 0;
    while (i + len < n) {
      const char c = meta[i + len];
      auto it = trie_[static_cast<size_t>(node)].next.find(c);
      if (it == trie_[static_cast<size_t>(node)].next.end())
        break;
      node = it->second;
      ++len;
      const int32_t piece = trie_[static_cast<size_t>(node)].piece;
      if (piece >= 0) {
        const float score =
            best[i] + pieces_[static_cast<size_t>(piece)].score;
        if (score > best[i + len] ||
            (score == best[i + len] && len > bestLen)) {
          best[i + len] = score;
          prevPiece[i + len] = piece;
          prevPos[i + len] = static_cast<int32_t>(i);
          bestPiece = piece;
          bestLen = len;
        }
      }
    }
    if (bestPiece < 0 && i + 1 <= n) {
      const float score = best[i] + -100.0F;
      if (score > best[i + 1]) {
        best[i + 1] = score;
        prevPiece[i + 1] = unk_;
        prevPos[i + 1] = static_cast<int32_t>(i);
      }
    }
  }

  std::vector<int32_t> ids;
  ids.reserve(64);
  for (int32_t pos = static_cast<int32_t>(n); pos > 0;
       pos = prevPos[static_cast<size_t>(pos)])
    ids.push_back(prevPiece[static_cast<size_t>(pos)]);
  std::reverse(ids.begin(), ids.end());

  const int budget = maxLen - 2;
  if (budget > 0 && static_cast<int>(ids.size()) > budget)
    ids.resize(static_cast<size_t>(budget));

  std::vector<int32_t> out;
  out.reserve(ids.size() + 2);
  out.push_back(bos_);
  out.insert(out.end(), ids.begin(), ids.end());
  out.push_back(eos_);
  return out;
}
