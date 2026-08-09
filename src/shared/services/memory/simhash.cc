#include "simhash.hxx"

#include <cctype>
#include <string>

namespace
{

uint64_t fnv1a(const char* data, size_t len)
{
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<unsigned char>(data[i]);
    h *= 1099511628211ULL;
  }
  return h;
}

} // namespace

uint64_t SimHash::hash(const std::string& text)
{
  std::string norm;
  norm.reserve(text.size());
  bool lastSpace = true;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      if (!lastSpace)
        norm += ' ';
      lastSpace = true;
    }
    else {
      norm += static_cast<char>(std::tolower(c));
      lastSpace = false;
    }
  }

  if (norm.size() < 3)
    return 0;

  int64_t bits[64] = {0};
  for (size_t i = 0; i + 3 <= norm.size(); ++i) {
    const uint64_t h = fnv1a(norm.data() + i, 3);
    for (int b = 0; b < 64; ++b)
      bits[b] += (h & (1ULL << b)) ? 1 : -1;
  }

  uint64_t out = 0;
  for (int b = 0; b < 64; ++b) {
    if (bits[b] > 0)
      out |= (1ULL << b);
  }
  return out;
}

int SimHash::distance(uint64_t a, uint64_t b)
{
  return __builtin_popcountll(a ^ b);
}
