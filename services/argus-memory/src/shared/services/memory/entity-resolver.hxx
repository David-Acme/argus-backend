#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <shared/utils/text-match/phrase-automaton.hxx>
#include <string>
#include <vector>

class SqliteGraph;

class EntityResolver
{
public:
  explicit EntityResolver(SqliteGraph& graph) : graph_(graph) {}

  void build();

  struct EntityHit
  {
    int64_t entityId;
    std::string surface;
    std::string kind;
    std::string personFrame;
    std::string catalog;
    int64_t catalogId;
  };

  std::vector<EntityHit> resolve(const std::string& text) const;

private:
  struct GazetteerEntry
  {
    std::string norm;
    std::string surface;
    std::string kind;
    std::string personFrame;
    int64_t entityId;
    std::string catalog;
    int64_t catalogId;
  };

  struct Snapshot
  {
    std::vector<GazetteerEntry> entries;
    std::shared_ptr<const text_match::PhraseAutomaton> automaton;
  };

  SqliteGraph& graph_;
  MemoryGraphRepository repo_;
  mutable std::mutex snapshotMutex_;
  std::shared_ptr<const Snapshot> snapshot_;

  std::shared_ptr<const Snapshot> currentSnapshot() const;
};
