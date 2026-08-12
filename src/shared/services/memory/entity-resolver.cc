#include "entity-resolver.hxx"

#include <algorithm>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/utils/text-norm/text-norm.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>

void EntityResolver::build()
{
  auto snapshot = std::make_shared<Snapshot>();
  std::vector<text_match::PatternRef> patterns;

  const auto add = [&](GazetteerEntry entry) {
    if (entry.norm.empty())
      return;
    patterns.push_back(
        {.classId = text_match::PatternClass::Entity,
         .payloadId = static_cast<uint32_t>(snapshot->entries.size()),
         .text = entry.norm});
    snapshot->entries.push_back(std::move(entry));
  };

  {
    std::scoped_lock lock(graph_.mutex());
    sqlite3* db = graph_.handle();
    if (!db) {
      snapshot->automaton = text_match::PhraseAutomaton::build({});
      {
        std::scoped_lock swap(snapshotMutex_);
        snapshot_ = snapshot;
      }
      return;
    }

    for (const auto& row : repo_.gazetteerAliases(db)) {
      add({.norm = row.norm,
           .surface = row.surface,
           .kind = row.kind,
           .personFrame = row.personFrame,
           .entityId = row.entityId,
           .catalog = "alias",
           .catalogId = 0});
    }
    const auto addCatalog = [&](const CatalogRow& row, const char* catalog) {
      add({.norm = text_norm::whitespace(row.surface),
           .surface = row.surface,
           .kind = row.kind,
           .personFrame = "none",
           .entityId = 0,
           .catalog = catalog,
           .catalogId = row.catalogId});
    };
    for (const auto& row : repo_.catalogPersons(db))
      addCatalog(row, "person");
    for (const auto& row : repo_.catalogCameras(db))
      addCatalog(row, "camera");
    for (const auto& row : repo_.catalogZones(db))
      addCatalog(row, "zone");
    for (const auto& row : repo_.catalogStreams(db))
      addCatalog(row, "camera_stream");
  }

  snapshot->automaton = text_match::PhraseAutomaton::build(patterns);
  {
    std::scoped_lock swap(snapshotMutex_);
    snapshot_ = snapshot;
  }
}

std::shared_ptr<const EntityResolver::Snapshot>
EntityResolver::currentSnapshot() const
{
  std::scoped_lock lock(snapshotMutex_);
  return snapshot_;
}

std::vector<EntityResolver::EntityHit>
EntityResolver::resolve(const std::string& text) const
{
  std::vector<EntityHit> hits;
  const auto snapshot = currentSnapshot();
  if (!snapshot || !snapshot->automaton)
    return hits;

  const std::string norm = text_norm::whitespace(text);
  if (norm.empty())
    return hits;

  thread_local text_match::MatchBuffer buffer;
  buffer.clear();
  snapshot->automaton->match(norm, buffer);
  if (buffer.items.empty())
    return hits;

  std::sort(buffer.items.begin(), buffer.items.end(),
            [](const text_match::Match& a, const text_match::Match& b) {
              if (a.begin != b.begin)
                return a.begin < b.begin;
              return (a.end - a.begin) > (b.end - b.begin);
            });

  uint32_t consumed = 0;
  for (const text_match::Match& match : buffer.items) {
    if (match.begin < consumed)
      continue;
    const auto& pattern = snapshot->automaton->pattern(match.patternIndex);
    if (pattern.payloadId >= snapshot->entries.size())
      continue;
    const GazetteerEntry& entry = snapshot->entries[pattern.payloadId];

    const bool seen =
        std::any_of(hits.begin(), hits.end(), [&entry](const EntityHit& hit) {
          return hit.catalog == entry.catalog &&
                 hit.catalogId == entry.catalogId &&
                 hit.entityId == entry.entityId;
        });
    if (!seen) {
      hits.push_back({.entityId = entry.entityId,
                      .surface = entry.surface,
                      .kind = entry.kind,
                      .personFrame = entry.personFrame,
                      .catalog = entry.catalog,
                      .catalogId = entry.catalogId});
    }
    consumed = match.end;
  }
  return hits;
}
