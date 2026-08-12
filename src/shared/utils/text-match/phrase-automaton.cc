#include <algorithm>
#include <cstdint>
#include <map>
#include <queue>
#include <shared/utils/text-match/phrase-automaton.hxx>
#include <utility>

namespace text_match
{

namespace
{

constexpr uint32_t kRoot = 0;
constexpr uint32_t kMaxPatterns = 65534;

bool isWordChar(unsigned char c)
{
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

struct ScratchNode
{
  std::map<uint8_t, uint32_t> children;
  std::vector<uint32_t> outputs;
  uint32_t fail = 0;
};

} // namespace

std::shared_ptr<const PhraseAutomaton>
PhraseAutomaton::build(const std::vector<PatternRef>& refs)
{
  auto out = std::make_shared<PhraseAutomaton>();
  out->nodes_.emplace_back();
  if (refs.empty() || refs.size() > kMaxPatterns)
    return out;

  std::vector<ScratchNode> scratch;
  scratch.reserve(refs.size() * 8);
  scratch.emplace_back();

  out->patterns_.reserve(refs.size());
  for (const PatternRef& ref : refs) {
    if (ref.text.empty())
      continue;
    uint32_t node = kRoot;
    size_t len = 0;
    for (const unsigned char c : ref.text) {
      uint32_t& child = scratch[node].children[c];
      if (child == 0) {
        child = static_cast<uint32_t>(scratch.size());
        scratch.emplace_back();
      }
      node = child;
      ++len;
    }
    if (len == 0 || len >= 65536)
      continue;
    scratch[node].outputs.push_back(
        static_cast<uint32_t>(out->patterns_.size()));
    out->patterns_.push_back({.classId = ref.classId,
                              .length = static_cast<uint16_t>(len),
                              .payloadId = ref.payloadId});
  }

  std::queue<uint32_t> bfs;
  for (const auto& entry : scratch[kRoot].children) {
    scratch[entry.second].fail = kRoot;
    bfs.push(entry.second);
  }
  while (!bfs.empty()) {
    const uint32_t node = bfs.front();
    bfs.pop();
    for (const auto& entry : scratch[node].children) {
      uint32_t f = scratch[node].fail;
      while (f != kRoot) {
        const auto it = scratch[f].children.find(entry.first);
        if (it != scratch[f].children.end()) {
          f = it->second;
          break;
        }
        f = scratch[f].fail;
      }
      if (f == kRoot) {
        const auto it = scratch[kRoot].children.find(entry.first);
        if (it != scratch[kRoot].children.end())
          f = it->second;
      }
      scratch[entry.second].fail = f;
      bfs.push(entry.second);
    }
  }

  const size_t nodeCount = scratch.size();
  out->nodes_.resize(nodeCount);
  for (size_t i = 0; i < nodeCount; ++i) {
    Node& n = out->nodes_[i];
    n.fail = scratch[i].fail;
    n.transStart = static_cast<uint32_t>(out->transByte_.size());
    n.transCount = static_cast<uint16_t>(scratch[i].children.size());
    for (const auto& entry : scratch[i].children) {
      out->transByte_.push_back(entry.first);
      out->transNext_.push_back(entry.second);
    }
  }

  std::queue<uint32_t> order;
  order.push(kRoot);
  while (!order.empty()) {
    const uint32_t node = order.front();
    order.pop();
    Node& n = out->nodes_[node];
    n.outStart = static_cast<uint32_t>(out->output_.size());
    const std::vector<uint32_t>& own = scratch[node].outputs;
    out->output_.insert(out->output_.end(), own.begin(), own.end());
    const uint32_t f = n.fail;
    if (f != kRoot) {
      const Node& fn = out->nodes_[f];
      const size_t start = fn.outStart;
      for (size_t i = 0; i < fn.outCount; ++i)
        out->output_.push_back(out->output_[start + i]);
    }
    n.outCount = static_cast<uint16_t>(out->output_.size() - n.outStart);
    for (const auto& entry : scratch[node].children)
      order.push(entry.second);
  }

  return out;
}

void PhraseAutomaton::match(std::string_view text, MatchBuffer& out) const
{
  if (nodes_.empty() || text.empty())
    return;
  uint32_t state = kRoot;
  const size_t size = text.size();
  for (size_t pos = 0; pos < size; ++pos) {
    const uint8_t c = static_cast<uint8_t>(text[pos]);
    while (state != kRoot) {
      const Node& node = nodes_[state];
      const uint8_t* base = transByte_.data() + node.transStart;
      const uint8_t* end = base + node.transCount;
      const uint8_t* it = std::lower_bound(base, end, c);
      if (it != end && *it == c)
        break;
      state = node.fail;
    }
    {
      const Node& node = nodes_[state];
      const uint8_t* base = transByte_.data() + node.transStart;
      const uint8_t* end = base + node.transCount;
      const uint8_t* it = std::lower_bound(base, end, c);
      if (it != end && *it == c)
        state = transNext_[node.transStart + (it - base)];
      else
        state = kRoot;
    }
    const Node& cur = nodes_[state];
    if (cur.outCount == 0)
      continue;
    const uint32_t outStart = cur.outStart;
    const uint32_t outCount = cur.outCount;
    for (uint32_t k = 0; k < outCount; ++k) {
      const uint32_t patternIndex = output_[outStart + k];
      const Pattern& p = patterns_[patternIndex];
      const size_t begin = pos + 1 - p.length;
      const size_t end = pos + 1;
      if (begin > 0 && isWordChar(static_cast<uint8_t>(text[begin - 1])))
        continue;
      if (end < size && isWordChar(static_cast<uint8_t>(text[end])))
        continue;
      if (!isWordChar(static_cast<uint8_t>(text[begin])) ||
          !isWordChar(static_cast<uint8_t>(text[end - 1])))
        continue;
      out.items.push_back({.patternIndex = patternIndex,
                           .begin = static_cast<uint32_t>(begin),
                           .end = static_cast<uint32_t>(end)});
    }
  }
}

size_t PhraseAutomaton::bytesUsed() const
{
  return nodes_.capacity() * sizeof(Node) + transByte_.capacity() +
         transNext_.capacity() * sizeof(uint32_t) +
         output_.capacity() * sizeof(uint32_t) +
         patterns_.capacity() * sizeof(Pattern);
}

} // namespace text_match
