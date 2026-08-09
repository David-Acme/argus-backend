#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class UnigramTokenizer
{
public:
  bool load(const std::string& jsonPath);

  std::vector<int32_t> encode(const std::string& text, int maxLen) const;

  int32_t bosId() const { return bos_; }
  int32_t eosId() const { return eos_; }
  int32_t padId() const { return pad_; }

private:
  struct TrieNode
  {
    std::unordered_map<char, int32_t> next;
    int32_t piece = -1;
  };

  struct Piece
  {
    int32_t id;
    float score;
  };

  std::vector<TrieNode> trie_;
  std::vector<Piece> pieces_;
  int32_t bos_ = 0;
  int32_t eos_ = 2;
  int32_t pad_ = 1;
  int32_t unk_ = 3;
};
