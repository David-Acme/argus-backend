#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared/enums.hxx>
#include <shared/utils/text-match/phrase-automaton.hxx>
#include <string>
#include <string_view>
#include <vector>

struct PhraseHit
{
  PhraseKind kind;
  MemoryType memoryType;
  uint32_t begin;
  uint32_t end;
};

// Aho-Corasick automaton over the static per-language vocabulary
// (src/shared/vocabulary/). No DB dependency.
class PhraseCatalog
{
public:
  PhraseCatalog() = default;

  void build();
  void reload() { build(); }
  void match(std::string_view lowered, const std::string& lang,
             std::vector<PhraseHit>& out) const;
  size_t phraseCount() const;

private:
  struct Entry
  {
    PhraseKind kind;
    MemoryType memoryType;
    std::string lang;
  };

  struct Snapshot
  {
    std::vector<Entry> entries;
    std::shared_ptr<const text_match::PhraseAutomaton> automaton;
  };

  std::shared_ptr<const Snapshot> currentSnapshot() const;

  mutable std::mutex snapshotMutex_;
  std::shared_ptr<const Snapshot> snapshot_;
};
