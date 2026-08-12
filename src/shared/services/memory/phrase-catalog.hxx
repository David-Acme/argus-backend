#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared/enums.hxx>
#include <shared/repositories/memory-phrase/memory-phrase-repository.hxx>
#include <shared/utils/text-match/phrase-automaton.hxx>
#include <string>
#include <string_view>
#include <vector>

class SqliteGraph;

struct PhraseHit
{
  PhraseKind kind;
  MemoryType memoryType;
  uint32_t begin;
  uint32_t end;
};

class PhraseCatalog
{
public:
  explicit PhraseCatalog(SqliteGraph& graph) : graph_(graph) {}

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

  SqliteGraph& graph_;
  MemoryPhraseRepository repo_;
  mutable std::mutex snapshotMutex_;
  std::shared_ptr<const Snapshot> snapshot_;
};
