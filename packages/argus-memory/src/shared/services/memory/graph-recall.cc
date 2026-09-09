#include "graph-recall.hxx"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>
#include <utility>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/memory/memory-vec.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/utils/text-norm/text-norm.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>

namespace
{

bool mentionsFirstPerson(const std::string& text)
{
  const std::string norm = text_norm::whitespace(text);
  const std::vector<std::string> markers = {" me ", " yo ", " mi ", " mis "};
  for (const auto& marker : markers) {
    if (norm.find(marker) != std::string::npos)
      return true;
  }
  return norm == "me" || norm == "yo" || norm == "mi";
}

constexpr int kBackgroundSample = 2;

std::string toSecondPerson(const std::string& text, bool es)
{
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 15>
      kEs{{{"conmigo", "contigo"}, {"mis ", "tus "}, {"mi ", "tu "},
      {"mí ", "ti "},   {"me ", "te "},   {"yo ", "tú "}, {"Mi ", "Tu "},
      {"Mis ", "Tus "}, {"Mí ", "Ti "},   {"Me ", "Te "}, {"Yo ", "Tú "},
      {"mi", "tu"},     {"mí", "ti"},     {"me", "te"},   {"yo", "tú"}}};
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 8>
      kEn{{{"my ", "your "}, {"My ", "Your "}, {"mine", "yours"},
      {"me ", "you "}, {"Me ", "You "}, {"I ", "you "}, {"I", "you"},
      {"me", "you"}}};
  const std::span<const std::pair<std::string_view, std::string_view>> table =
      es ? std::span<const std::pair<std::string_view, std::string_view>>{kEs}
         : std::span<const std::pair<std::string_view, std::string_view>>{kEn};
  const size_t count = table.size();

  const auto rightBoundary = [&](size_t after) {
    if (after >= text.size())
      return true;
    const char c = text[after];
    return c == ',' || c == '.' || c == ';' || c == ':' || c == '?' ||
           c == '!' || c == '\n';
  };

  std::string out;
  out.reserve(text.size() + 8);
  size_t i = 0;
  while (i < text.size()) {
    const bool atWordStart = i == 0 || text[i - 1] == ' ' || text[i - 1] == ',';
    bool shifted = false;
    if (atWordStart) {
      for (size_t k = 0; k < count; ++k) {
        const std::string_view from(table[k].first);
        if (text.compare(i, from.size(), from) != 0)
          continue;
        if (from.back() != ' ' && !rightBoundary(i + from.size()))
          continue;
        out += table[k].second;
        i += from.size();
        shifted = true;
        break;
      }
    }
    if (!shifted) {
      out += text[i];
      ++i;
    }
  }
  return out;
}

float configFloat(const char* key, float fallback)
{
  const double value = ConfigService::getDouble(key);
  return value > 0.0 ? static_cast<float>(value) : fallback;
}

int configInt(const char* key, int fallback)
{
  const int value = ConfigService::getInt(key);
  return value > 0 ? value : fallback;
}

bool worthSemanticSearch(const std::string& text)
{
  if (text.find('?') != std::string::npos ||
      text.find("\xc2\xbf") != std::string::npos)
    return true;
  size_t words = 0;
  bool inWord = false;
  for (const char c : text) {
    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    if (space) {
      inWord = false;
      continue;
    }
    if (!inWord) {
      ++words;
      inWord = true;
    }
  }
  return words >= 4;
}

bool mentionsAnaphora(const std::string& text)
{
  static constexpr std::array<std::string_view, 8> kEs{
      "ella", "eso", "aquello", "ese", "esa", "esto", "aquella", "aquel"};
  static constexpr std::array<std::string_view, 8> kEn{
      "she", "he", "it", "they", "them", "him", "her", "that"};
  const std::vector<std::string> tokens = text_norm::words(text, 2);
  for (const auto& token : tokens) {
    for (const std::string_view p : kEs)
      if (token == p)
        return true;
    for (const std::string_view p : kEn)
      if (token == p)
        return true;
  }
  return false;
}

std::string stripTagChars(std::string text)
{
  text.erase(std::remove_if(text.begin(), text.end(),
                            [](char c) { return c == '<' || c == '>'; }),
             text.end());
  return text;
}

} // namespace

std::string GraphRecall::render(int64_t entityId, int64_t addresseeEntityId,
                                const std::string& canonical) const
{
  if (entityId <= 0)
    return canonical;

  std::vector<AliasInfo> aliases;
  {
    std::scoped_lock lock(graph_.mutex());
    aliases = graph_.aliasesForEntity(entityId);
  }
  if (aliases.empty())
    return canonical;

  const auto pick = [&](const std::string& frame) -> std::string {
    for (const auto& alias : aliases)
      if (alias.personFrame == frame)
        return alias.surface;
    return {};
  };

  std::string surface;
  if (addresseeEntityId > 0 && entityId == addresseeEntityId) {
    surface = pick("second");
    if (surface.empty())
      surface = pick("first");
  }
  else {
    surface = pick("none");
    if (surface.empty())
      surface = pick("second");
    if (surface.empty())
      surface = pick("first");
  }
  if (surface.empty())
    return canonical;
  const std::string normSurface = text_norm::whitespace(surface);
  const std::string normCanonical = text_norm::whitespace(canonical);
  if (!normSurface.empty() &&
      normCanonical.find(normSurface) != std::string::npos)
    return canonical;
  return surface + ": " + canonical;
}

const GraphRecall::Tuning& GraphRecall::tuning() const
{
  if (!tuning_) {
    tuning_ = Tuning{
        .margin = configFloat("memory.vector_margin", 0.05F),
        .floorSim = configFloat("memory.vector_min_sim", 0.80F),
        .strictSim = configFloat("memory.vector_strict_min_sim", 0.86F),
        .smallStoreSim = configFloat("memory.vector_small_store_sim", 0.84F),
        .maxFacts = configInt("memory.semantic_max_facts", 2),
        .minHits = configInt("memory.lexical_min_hits", 4)};
  }
  return *tuning_;
}

void GraphRecall::collectSemantic(const GraphRecallInput& input,
                                  GraphRecallResult& result)
{
  if (!worthSemanticSearch(input.text))
    return;

  const auto query = embedding_.embed(input.text, "query:");
  if (!query)
    return;

  const std::string encoded = memory_vec::encode(*query);
  const std::string partition =
      memory_vec::partitionFor(input.scope, input.refId);
  std::vector<VecNeighbour> neighbours;
  {
    std::scoped_lock lock(vecDb_.mutex());
    sqlite3* db = vecDb_.handle();
    if (!db)
      return;
    neighbours = repo_.vecNeighbours(
        db, {.encoded = encoded,
             .partition = partition,
             .k = std::max(kBackgroundSample, input.limit * 3)});
  }

  std::vector<VecNeighbour> best;
  size_t distinctFacts = 0;
  {
    std::vector<int64_t> seen;
    for (const auto& neighbour : neighbours) {
      if (std::find(seen.begin(), seen.end(), neighbour.factId) != seen.end())
        continue;
      seen.push_back(neighbour.factId);
      ++distinctFacts;
    }
    for (const auto& neighbour : neighbours) {
      const float sim = 1.0F - neighbour.distance;
      bool merged = false;
      for (auto& kept : best) {
        if (kept.factId != neighbour.factId)
          continue;
        merged = true;
        if (sim > kept.distance)
          kept.distance = sim;
        break;
      }
      if (!merged)
        best.push_back({.factId = neighbour.factId, .distance = sim});
    }
  }
  if (best.empty())
    return;

  std::sort(best.begin(), best.end(),
            [](const VecNeighbour& a, const VecNeighbour& b) {
              return a.distance > b.distance;
            });

  float sum = 0.0F;
  for (const auto& row : best)
    sum += row.distance;

  const Tuning& cfg = tuning();
  const float margin = cfg.margin;
  const float floorSim = cfg.floorSim;
  const float strictSim = cfg.strictSim;
  const int maxFacts = cfg.maxFacts;

  // Small stores have no reliable background: the absolute strict floor decides.
  const bool strictGate = distinctFacts < 3;
  const float strictFloor = distinctFacts <= 1 ? strictSim : cfg.smallStoreSim;

  int taken = 0;
  for (const auto& neighbour : best) {
    if (taken >= maxFacts ||
        static_cast<int>(result.hits.size()) >= input.limit)
      break;
    const float sim = neighbour.distance;
    if (sim < floorSim)
      break;
    if (strictGate) {
      if (sim < strictFloor)
        continue;
    }
    else {
      const float others =
          (sum - sim) / static_cast<float>(best.size() - 1);
      if (sim - others < margin)
        continue;
    }

    bool seen = false;
    for (const auto& existing : result.hits) {
      if (existing.factId == neighbour.factId) {
        seen = true;
        break;
      }
    }
    if (seen)
      continue;

    {
      std::optional<RecallHit> fact;
      std::optional<EpisodeHit> episode;
      {
        std::scoped_lock lock(graph_.mutex());
        fact = repo_.factById(graph_.handle(), {.factId = neighbour.factId,
                                                .scope = input.scope,
                                                .refId = input.refId});
        if (!fact)
          episode = repo_.episodeById(graph_.handle(),
                                      {.episodeId = neighbour.factId,
                                       .scope = input.scope,
                                       .refId = input.refId});
      }
      if (fact) {
        GraphRecallHit out;
        out.factId = fact->factId;
        out.entityId = fact->entityId;
        out.predicate = fact->predicate;
        out.value = fact->value;
        out.canonical = fact->canonical;
        out.type = fact->type;
        out.priority = fact->priority;
        out.confidence = fact->confidence;
        out.hops = 1;
        out.score = static_cast<float>(fact->priority) * sim;
        out.rendered =
            render(out.entityId, input.addresseeEntityId, out.canonical);
        result.hits.push_back(std::move(out));
        ++taken;
        continue;
      }
      if (episode) {
        GraphRecallHit out;
        out.factId = 0;
        out.entityId = 0;
        out.canonical = episode->summary;
        out.type = "episode";
        out.priority = static_cast<int>(episode->salience * 100.0F);
        out.hops = 1;
        out.score = static_cast<float>(episode->salience * 100.0F) * sim;
        out.rendered = episode->summary;
        result.hits.push_back(std::move(out));
        result.usedEpisodeIds.push_back(episode->episodeId);
        ++taken;
      }
    }
  }
}

GraphRecallResult GraphRecall::recall(const GraphRecallInput& input)
{
  GraphRecallResult result;
  if (input.text.empty())
    return result;

  const auto deadline = [&] {
    const int ms = ConfigService::getInt("memory.recall_deadline_ms");
    return std::chrono::steady_clock::now() +
           std::chrono::milliseconds(ms > 0 ? ms : 200);
  }();
  const auto overDeadline = [&] {
    return std::chrono::steady_clock::now() >= deadline;
  };

  const auto collect = [&](int64_t entityId) {
    std::vector<RecallHit> facts;
    {
      std::scoped_lock lock(graph_.mutex());
      facts = graph_.factsForEntity({.entityId = entityId,
                                     .scope = input.scope,
                                     .refId = input.refId,
                                     .maxHops = input.maxHops,
                                     .limit = input.limit});
    }
    for (const auto& fact : facts) {
      bool seen = false;
      for (const auto& existing : result.hits) {
        if (existing.factId == fact.factId) {
          seen = true;
          break;
        }
      }
      if (seen)
        continue;
      GraphRecallHit out;
      out.factId = fact.factId;
      out.entityId = fact.entityId;
      out.predicate = fact.predicate;
      out.value = fact.value;
      out.canonical = fact.canonical;
      out.type = fact.type;
      out.priority = fact.priority;
      out.confidence = fact.confidence;
      out.hops = fact.hops;
      out.score = fact.score;
      out.rendered =
          render(fact.entityId, input.addresseeEntityId, fact.canonical);
      result.hits.push_back(std::move(out));
    }
  };

  resolver_.build();
  for (const auto& hit : resolver_.resolve(input.text)) {
    if (hit.entityId <= 0)
      continue;
    result.resolvedEntityIds.push_back(hit.entityId);
    collect(hit.entityId);
  }
  if (input.addresseeEntityId > 0 && mentionsFirstPerson(input.text)) {
    bool known = false;
    for (int64_t id : result.resolvedEntityIds) {
      if (id == input.addresseeEntityId) {
        known = true;
        break;
      }
    }
    if (!known) {
      result.resolvedEntityIds.push_back(input.addresseeEntityId);
      collect(input.addresseeEntityId);
    }
  }
  if (result.resolvedEntityIds.empty() && mentionsAnaphora(input.text) &&
      !input.activeEntityIds.empty()) {
    const int64_t last = input.activeEntityIds.back();
    result.resolvedEntityIds.push_back(last);
    collect(last);
  }
  result.entityAnchored = !result.hits.empty();

  if (result.hits.empty()) {
    const auto tokens = text_norm::words(input.text);
    if (!tokens.empty()) {
      std::string match;
      for (const auto& token : tokens) {
        if (!match.empty())
          match += " OR ";
        match += "\"" + token + "\"";
      }
      std::vector<RecallHit> facts;
      {
        std::scoped_lock lock(graph_.mutex());
        facts = repo_.ftsFacts(graph_.handle(),
                               {.match = match,
                                .scope = input.scope,
                                .refId = input.refId,
                                .limit = input.limit});
      }
      for (const auto& fact : facts) {
        GraphRecallHit out;
        out.factId = fact.factId;
        out.entityId = fact.entityId;
        out.predicate = fact.predicate;
        out.value = fact.value;
        out.canonical = fact.canonical;
        out.type = fact.type;
        out.priority = fact.priority;
        out.confidence = fact.confidence;
        out.hops = 1;
        out.score = static_cast<float>(out.priority);
        out.rendered =
            render(out.entityId, input.addresseeEntityId, out.canonical);
        result.hits.push_back(std::move(out));
      }
    }
  }

  const bool underBudget =
      static_cast<int>(result.hits.size()) < tuning().minHits;

  bool weakLexical = false;
  if (!underBudget && !result.entityAnchored && !result.hits.empty()) {
    const auto queryWords = text_norm::wordSet(input.text, 3);
    const auto topWords = text_norm::wordSet(result.hits.front().canonical, 3);
    size_t shared = 0;
    for (const auto& word : queryWords)
      if (topWords.count(word))
        ++shared;
    weakLexical = shared < 2;
  }

  if ((underBudget || weakLexical) && !overDeadline())
    collectSemantic(input, result);

  if (underBudget && worthSemanticSearch(input.text) && !overDeadline()) {
    const auto tokens = text_norm::words(input.text);
    if (!tokens.empty()) {
      std::string match;
      for (const auto& token : tokens) {
        if (!match.empty())
          match += " OR ";
        match += "\"" + token + "\"";
      }
      std::vector<EpisodeHit> episodes;
      {
        std::scoped_lock lock(graph_.mutex());
        episodes = repo_.ftsEpisodes(graph_.handle(),
                                     {.match = match,
                                      .scope = input.scope,
                                      .refId = input.refId,
                                      .limit = 2});
      }
      for (const auto& episode : episodes) {
        bool seen = false;
        for (const auto& existing : result.hits)
          if (existing.type == "episode" &&
              existing.canonical == episode.summary)
            seen = true;
        if (seen)
          continue;
        GraphRecallHit out;
        out.factId = 0;
        out.entityId = 0;
        out.canonical = episode.summary;
        out.type = "episode";
        out.priority = static_cast<int>(episode.salience * 100.0F);
        out.hops = 1;
        out.score = static_cast<float>(episode.salience * 100.0F) *
                    (1.0F + 0.05F * static_cast<float>(episode.hitCount));
        out.rendered = episode.summary;
        result.hits.push_back(std::move(out));
        result.usedEpisodeIds.push_back(episode.episodeId);
      }
    }
  }

  if (result.hits.empty())
    return result;

  std::sort(result.hits.begin(), result.hits.end(),
            [](const GraphRecallHit& a, const GraphRecallHit& b) {
              return a.score > b.score;
            });

  const bool es = input.lang.empty() || input.lang == "es";
  std::string block = es ? "<memorias>\n" : "<memories>\n";
  const int maxTokens = ConfigService::getInt("memory.recall_max_tokens");
  const size_t budget =
      maxTokens > 0 ? static_cast<size_t>(maxTokens * 4) : 2048;
  for (const auto& hit : result.hits) {
    const std::string line =
        "- " + toSecondPerson(stripTagChars(hit.rendered), es) + "\n";
    if (block.size() + line.size() > budget)
      break;
    block += line;
    if (hit.factId > 0)
      result.usedIds.push_back(hit.factId);
  }
  block += es ? "</memorias>\nUsa estos datos para responder.\n"
              : "</memories>\nUse these facts to answer.\n";
  result.block = std::move(block);
  return result;
}
