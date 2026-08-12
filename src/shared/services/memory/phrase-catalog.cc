#include "phrase-catalog.hxx"

#include <shared/services/memory/sqlite-graph.hxx>

namespace
{

text_match::PatternClass classFor(PhraseKind kind)
{
  switch (kind) {
    case PhraseKind::Confirmation:
      return text_match::PatternClass::Connector;
    case PhraseKind::StatementStart:
      return text_match::PatternClass::Kinship;
    case PhraseKind::RecallMarker:
      return text_match::PatternClass::Stopword;
    default:
      return text_match::PatternClass::Predicate;
  }
}

} // namespace

void PhraseCatalog::build()
{
  auto snapshot = std::make_shared<Snapshot>();
  std::vector<text_match::PatternRef> patterns;

  {
    std::scoped_lock lock(graph_.mutex());
    for (auto& row : repo_.allPhrases(graph_.handle())) {
      if (row.phrase.empty())
        continue;
      patterns.push_back(
          {.classId = classFor(row.kind),
           .payloadId = static_cast<uint32_t>(snapshot->entries.size()),
           .text = row.phrase});
      snapshot->entries.push_back({.kind = row.kind,
                                   .memoryType = row.memoryType,
                                   .lang = std::move(row.lang)});
    }
  }

  snapshot->automaton = text_match::PhraseAutomaton::build(patterns);
  {
    std::scoped_lock swap(snapshotMutex_);
    snapshot_ = snapshot;
  }
}

std::shared_ptr<const PhraseCatalog::Snapshot>
PhraseCatalog::currentSnapshot() const
{
  std::scoped_lock lock(snapshotMutex_);
  return snapshot_;
}

size_t PhraseCatalog::phraseCount() const
{
  const auto snapshot = currentSnapshot();
  return snapshot ? snapshot->entries.size() : 0;
}

void PhraseCatalog::match(std::string_view lowered, const std::string& lang,
                          std::vector<PhraseHit>& out) const
{
  out.clear();
  const auto snapshot = currentSnapshot();
  if (!snapshot || !snapshot->automaton || lowered.empty())
    return;

  thread_local text_match::MatchBuffer buffer;
  buffer.clear();
  snapshot->automaton->match(lowered, buffer);

  const std::string& wanted = lang.empty() ? "es" : lang;
  for (const auto& hit : buffer.items) {
    const auto& pattern = snapshot->automaton->pattern(hit.patternIndex);
    const auto& entry = snapshot->entries[pattern.payloadId];
    if (entry.lang != wanted)
      continue;
    out.push_back({.kind = entry.kind,
                   .memoryType = entry.memoryType,
                   .begin = hit.begin,
                   .end = hit.end});
  }
}
