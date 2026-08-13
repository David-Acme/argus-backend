#include "memory-service.hxx"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <drogon/drogon.h>
#include <iostream>
#include <mutex>
#include <set>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/memory/memory-vec.hxx>
#include <shared/services/memory/rule-parser.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/utils/text-norm/text-norm.hxx>
#include <shared/vocabulary/vocabulary.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr const char* kCompactSystem =
    "A home assistant's memory core. Compress the conversation below into "
    "ONE short paragraph (3-6 sentences), written in the SAME language as "
    "the conversation. Keep only durable, reusable facts about the user and "
    "their home: people, schedules, preferences, allergies, names, routines, "
    "settings and instructions. Omit greetings, small talk, questions and "
    "anything transient. Write it in second person ('tu' / 'your'). Reply "
    "ONLY with the paragraph — no quotes, no labels, no preamble.";

std::vector<std::string> splitSentences(const std::string& text)
{
  std::vector<std::string> sentences;
  std::string current;
  for (size_t i = 0; i < text.size(); ++i) {
    current += text[i];
    const unsigned char c = static_cast<unsigned char>(text[i]);
    const bool boundary =
        c == '.' || c == '!' || c == '?' || c == ';' || c == ':' || c == '\n';
    if (boundary) {
      sentences.push_back(current);
      current.clear();
    }
  }
  if (!current.empty())
    sentences.push_back(current);
  return sentences;
}

std::vector<std::string> chunkContent(const std::string& content, int maxChars)
{
  if (content.size() <= static_cast<size_t>(maxChars))
    return {content};

  const auto sentences = splitSentences(content);
  if (sentences.size() <= 1)
    return {content};

  std::vector<std::string> chunks;
  std::string current;
  std::string overlap;
  for (const auto& sentence : sentences) {
    if (!current.empty() &&
        current.size() + sentence.size() > static_cast<size_t>(maxChars)) {
      chunks.push_back(current);
      current = overlap + sentence;
      overlap = sentence;
    }
    else {
      current += sentence;
      overlap = sentence;
    }
  }
  if (!current.empty())
    chunks.push_back(current);
  return chunks.empty() ? std::vector<std::string>{content} : chunks;
}

const std::vector<std::pair<std::string, std::string>>& builtinSynonyms()
{
  static const std::vector<std::pair<std::string, std::string>> table = {
      {"wifi", "internet red network"},
      {"internet", "wifi red network"},
      {"red", "wifi internet network"},
      {"perro", "can mascota dog"},
      {"gato", "cat mascota"},
      {"puerta", "door entrada"},
      {"camara", "camera camara vigilancia"},
      {"camera", "camara camara vigilancia"},
      {"alarma", "alarm"},
      {"termostato", "thermostat"},
      {"garaje", "garage"},
      {"hermana", "sister"},
      {"hermano", "brother"},
      {"esposa", "wife pareja"},
      {"marido", "husband pareja"},
      {"abuela", "grandmother"},
      {"abuelo", "grandfather"},
      {"domingos", "sunday domingo"},
      {"sabados", "saturday sabado"},
      {"sabado", "saturday"},
      {"domingo", "sunday"},
  };
  return table;
}

bool containsWord(const std::string& text, const std::string& word)
{
  size_t pos = 0;
  while ((pos = text.find(word, pos)) != std::string::npos) {
    const bool before =
        pos == 0 || !std::isalnum(static_cast<unsigned char>(text[pos - 1]));
    const size_t after = pos + word.size();
    const bool afterOk = after >= text.size() ||
                         !std::isalnum(static_cast<unsigned char>(text[after]));
    if (before && afterOk)
      return true;
    pos += word.size();
  }
  return false;
}

std::string expandForEmbedding(const std::string& content)
{
  std::string lower = content;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  std::set<std::string> extra;
  for (const auto& [term, syns] : builtinSynonyms()) {
    if (containsWord(lower, term)) {
      std::string cur;
      for (char c : syns) {
        if (c == ' ') {
          if (!cur.empty())
            extra.insert(cur);
          cur.clear();
        }
        else
          cur += c;
      }
      if (!cur.empty())
        extra.insert(cur);
    }
  }

  for (const auto& [term, syns] :
       ConfigService::getStringPairs("memory.synonyms")) {
    if (containsWord(lower, term))
      extra.insert(syns);
  }

  if (extra.empty())
    return content;
  std::string out = content;
  for (const auto& syn : extra)
    out += " " + syn;
  return out;
}

} // namespace

MemoryService::~MemoryService()
{
  shutdown();
}

void MemoryService::waitForIdle(int waitMs)
{
  if (waitMs <= 0)
    return;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
  while (!stop_ && llm_.isBusy() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void MemoryService::processExtract(const MemoryJob& job)
{
  const auto formed = formation_.observe({.channel = "user_turn",
                                          .text = job.text,
                                          .actor = {},
                                          .at = std::time(nullptr),
                                          .userId = job.userId,
                                          .lang = job.lang,
                                          .sessionId = {},
                                          .entitiesHint = {},
                                          .allowModel = true,
                                          .salient = job.salient});
  if (!formed)
    return;
  LOG_INFO << "MemoryService: deferred extraction stored fact "
           << formed->factId << " (" << formed->source << ")";
  enqueueJob({.kind = MemoryJob::Kind::Embed,
              .memoryId = formed->factId,
              .userId = 0,
              .text = {},
              .lang = {},
              .preferIdle = false,
              .salient = false,
              .episode = false});
}

void MemoryService::processCompact(const MemoryJob& job)
{
  if (job.preferIdle) {
    const int waitMs = ConfigService::getInt("memory.compact_wait_ms");
    waitForIdle(waitMs > 0 ? waitMs : 15000);
    if (llm_.isBusy()) {
      enqueueJob(job);
      return;
    }
  }
  if (!llm_.isLoaded())
    return;

  const int maxTokens = ConfigService::getInt("memory.compact_max_tokens");
  const ChatRequest req{
      .messages = {ChatMessage{.role = "system", .content = kCompactSystem},
                   ChatMessage{.role = "user",
                               .content = "Conversation:\n" + job.text}},
      .maxTokens = maxTokens > 0 ? maxTokens : 256,
      .temperature = 0.0F,
      .resetContext = true,
  };
  const std::string summary = llm_.chat(req);
  if (summary.empty()) {
    LOG_WARN << "MemoryService: compact produced an empty summary";
    return;
  }
  LOG_INFO << "MemoryService: compact done (" << summary.size() << " chars)";

  const int64_t id = [&] {
    std::scoped_lock lock(graph_->mutex());
    return graph_->recordEpisode({.kind = "compaction",
                                  .summary = summary,
                                  .actor = "",
                                  .occurredAt = std::time(nullptr),
                                  .sessionId = {},
                                  .lang = job.lang,
                                  .scope = "user",
                                  .refId = job.userId,
                                  .salience = 0.7F,
                                  .sourceId = std::nullopt,
                                  .mentionEntityIds = {}});
  }();
  if (id > 0)
    enqueueJob({.kind = MemoryJob::Kind::Embed,
                .memoryId = id,
                .userId = 0,
                .text = {},
                .lang = {},
                .preferIdle = false,
                .salient = false,
                .episode = true});
}

void MemoryService::processJob(const MemoryJob& job)
{
  switch (job.kind) {
    case MemoryJob::Kind::Embed:
      embedAndStore(job.memoryId, job.episode);
      break;
    case MemoryJob::Kind::Compact:
      processCompact(job);
      break;
    case MemoryJob::Kind::Rebuild:
      rebuildAll();
      break;
    case MemoryJob::Kind::Extract:
      processExtract(job);
      break;
    case MemoryJob::Kind::Profile:
      processProfile(job);
      break;
  }
}

void MemoryService::workerLoop()
{
  for (;;) {
    MemoryJob job;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueCv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_ && queue_.empty())
        return;
      working_.store(true);
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      processJob(job);
    }
    catch (const std::exception& e) {
      LOG_WARN << "MemoryService worker job failed: " << e.what();
    }
    working_.store(false);
  }
}

void MemoryService::startWorker()
{
  std::lock_guard<std::mutex> lock(queueMutex_);
  if (running_)
    return;
  stop_ = false;
  running_ = true;
  worker_ = std::thread(&MemoryService::workerLoop, this);
}

void MemoryService::stopWorker()
{
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    stop_ = true;
  }
  queueCv_.notify_all();
  if (worker_.joinable())
    worker_.join();
  running_ = false;
}

void MemoryService::enqueueJob(MemoryJob job)
{
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.push_back(std::move(job));
  }
  queueCv_.notify_one();
}

void MemoryService::init()
{
  graph_->open(ConfigService::getString("database.file"));
  graph_->migrateLegacy();
  vecDb_.applySchema();
  phrases_.build();
  extractor_.rebuild(vocabulary::allLexiconEntries());
  formation_.setExtractor(&extractor_);
  embedding_.init();
  startWorker();
  if (vecDb_.schemaOutdated()) {
    vecDb_.recreateMemoryVecTable();
    enqueueJob({.kind = MemoryJob::Kind::Rebuild,
                .memoryId = 0,
                .userId = 0,
                .text = {},
                .lang = {},
                .preferIdle = false,
                .salient = false,
                .episode = false});
  }
}

void MemoryService::shutdown()
{
  stopWorker();
  embedding_.shutdown();
  graph_->close();
}

bool MemoryService::isLoaded() const
{
  return embedding_.isLoaded();
}

int64_t MemoryService::captureInline(const InlineCapture& capture)
{
  const auto formed = formation_.observe({.channel = capture.channel,
                                          .text = capture.text,
                                          .actor = {},
                                          .at = std::time(nullptr),
                                          .userId = capture.userId,
                                          .lang = capture.lang,
                                          .sessionId = {},
                                          .entitiesHint = {},
                                          .allowModel = false,
                                          .salient = capture.salient});
  if (!formed)
    return 0;
  enqueueJob({.kind = MemoryJob::Kind::Embed,
              .memoryId = formed->factId,
              .userId = 0,
              .text = {},
              .lang = {},
              .preferIdle = false,
              .salient = false,
              .episode = false});
  return formed->factId;
}

void MemoryService::deferCapture(const InlineCapture& capture)
{
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    for (const auto& job : queue_)
      if (job.kind == MemoryJob::Kind::Extract && job.userId == capture.userId &&
          job.text == capture.text)
        return;
  }
  enqueueJob({.kind = MemoryJob::Kind::Extract,
              .memoryId = 0,
              .userId = capture.userId,
              .text = capture.text,
              .lang = capture.lang,
              .preferIdle = false,
              .salient = capture.salient,
              .episode = false});
}

CaptureResult MemoryService::captureExplicit(const CaptureInput& input)
{
  const InlineCapture capture{.channel = "user_turn",
                              .text = input.text,
                              .lang = input.lang,
                              .userId = input.userId,
                              .salient = false};
  const int64_t id = captureInline(capture);
  if (id > 0)
    return {.outcome = CaptureOutcome::Stored, .factId = id};
  if (!ruleParser_.parse({.text = input.text, .lang = input.lang}) &&
      !ruleParser_.parseStatement({.text = input.text, .lang = input.lang}))
    return {};
  deferCapture(capture);
  return {.outcome = CaptureOutcome::Deferred, .factId = 0};
}

CaptureResult MemoryService::captureImplicit(const CaptureInput& input)
{
  deferCapture({.channel = "user_turn",
                .text = input.text,
                .lang = input.lang,
                .userId = input.userId,
                .salient = true});
  return {.outcome = CaptureOutcome::Deferred, .factId = 0};
}

CaptureResult MemoryService::captureToolCall(int64_t userId,
                                             const std::string& lang,
                                             const ToolCall& call)
{
  const InlineCapture capture{.channel = "tool_call",
                              .text = call.content,
                              .lang = lang,
                              .userId = userId,
                              .salient = true};
  const int64_t id = captureInline(capture);
  if (id > 0)
    return {.outcome = CaptureOutcome::Stored, .factId = id};
  deferCapture(capture);
  return {.outcome = CaptureOutcome::Deferred, .factId = 0};
}

RecallContext MemoryService::recall(const RecallInput& input)
{
  RecallContext ctx;
  const int topK = ConfigService::getInt("memory.recall_top_k");
  const auto recalled = graphRecall_.recall({.text = input.text,
                                             .lang = input.lang,
                                             .scope = "user",
                                             .refId = input.userId,
                                             .maxHops = 1,
                                             .limit = topK > 0 ? topK : 4,
                                             .addresseeEntityId = 0,
                                             .activeEntityIds = {}});
  if (!recalled.hits.empty()) {
    ctx.prependText = recalled.block;
    ctx.usedIds = recalled.usedIds;
  }
  bumpEpisodeHits(recalled.usedEpisodeIds);
  return ctx;
}

void MemoryService::enqueueSummary(int64_t userId,
                                   const std::string& transcript,
                                   const std::string& lang)
{
  if (!running_ || userId < 0 || transcript.empty())
    return;
  if (!llm_.isLoaded())
    return;
  enqueueJob({.kind = MemoryJob::Kind::Compact,
              .memoryId = 0,
              .userId = userId,
              .text = transcript,
              .lang = lang,
              .preferIdle = false,
              .salient = false,
              .episode = false});
}

void MemoryService::enqueueCompaction(int64_t userId,
                                      const std::string& transcript,
                                      const std::string& lang)
{
  if (!running_ || userId < 0 || transcript.empty())
    return;
  if (!llm_.isLoaded())
    return;
  enqueueJob({.kind = MemoryJob::Kind::Compact,
              .memoryId = 0,
              .userId = userId,
              .text = transcript,
              .lang = lang,
              .preferIdle = true,
              .salient = false,
              .episode = false});
}

void MemoryService::flushPending()
{
  const int timeout = ConfigService::getInt("memory.flush_timeout_ms");
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout > 0 ? timeout : 60000);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (queue_.empty() && !working_.load())
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  LOG_WARN << "MemoryService::flushPending timed out with pending jobs";
}

void MemoryService::bumpHitCount(const std::vector<int64_t>& ids)
{
  std::scoped_lock lock(graph_->mutex());
  graph_->bumpFactHits(ids);
}

void MemoryService::bumpEpisodeHits(const std::vector<int64_t>& ids)
{
  if (ids.empty())
    return;
  std::scoped_lock lock(graph_->mutex());
  graphRepo_.bumpEpisodeHits(graph_->handle(), ids, std::time(nullptr));
}

int64_t MemoryService::resolveAddresseeEntity(const std::string& lang)
{
  std::scoped_lock lock(graph_->mutex());
  const auto found = graph_->resolveEntity(
      {.surface = "usuario", .norm = "usuario", .lang = lang});
  return found.value_or(0);
}

std::string MemoryService::profileFor(int64_t userId, const std::string& lang)
{
  if (userId < 0)
    return {};
  {
    std::lock_guard<std::mutex> lock(profileMutex_);
    const int stale = ConfigService::getInt("memory.profile_stale_seconds");
    const std::time_t maxAge =
        stale > 0 ? static_cast<std::time_t>(stale) : 300;
    if (profileCache_.userId == userId && profileCache_.lang == lang &&
        profileCache_.built > 0 &&
        std::time(nullptr) - profileCache_.built < maxAge)
      return profileCache_.text;
  }
  const std::string built = buildProfile(userId, lang);
  {
    std::lock_guard<std::mutex> lock(profileMutex_);
    profileCache_ = {.userId = userId,
                     .lang = lang,
                     .text = built,
                     .built = std::time(nullptr)};
  }
  if (running_ && llm_.isLoaded()) {
    enqueueJob({.kind = MemoryJob::Kind::Profile,
                .memoryId = 0,
                .userId = userId,
                .text = {},
                .lang = lang,
                .preferIdle = true,
                .salient = false,
                .episode = false});
  }
  return built;
}

std::string MemoryService::buildProfile(int64_t userId, const std::string& lang)
{
  std::vector<ProfileFactRow> rows;
  {
    std::scoped_lock lock(graph_->mutex());
    rows = graphRepo_.topProfileFacts(graph_->handle(), userId, 6);
  }
  if (rows.size() < 2)
    return {};

  std::vector<std::string> persona;
  std::vector<std::string> orders;
  for (const auto& row : rows) {
    if (row.type == "instruction")
      orders.push_back(row.canonical);
    else
      persona.push_back(row.canonical);
  }
  const bool es = lang.empty() || lang == "es";
  std::string out;
  if (!persona.empty()) {
    out += es ? "Datos sobre el usuario:\n" : "Facts about the user:\n";
    for (const auto& line : persona)
      out += "- " + text_norm::whitespace(line, false) + "\n";
  }
  if (!orders.empty()) {
    out += es ? "Peticiones permanentes:\n" : "Standing requests:\n";
    for (const auto& line : orders)
      out += "- " + text_norm::whitespace(line, false) + "\n";
  }
  return out;
}

void MemoryService::processProfile(const MemoryJob& job)
{
  const int waitMs = ConfigService::getInt("memory.compact_wait_ms");
  waitForIdle(waitMs > 0 ? waitMs : 15000);
  if (llm_.isBusy()) {
    enqueueJob(job);
    return;
  }
  if (!llm_.isLoaded())
    return;

  const std::string base = buildProfile(job.userId, job.lang);
  if (base.empty())
    return;
  const bool es = job.lang.empty() || job.lang == "es";
  const std::string system =
      es ? "Convierte los datos siguientes en un perfil breve del usuario, "
           "en segunda persona, dos o tres frases, en el mismo idioma, sin "
           "añadir nada que no esté en los datos."
         : "Turn the facts below into a short second-person profile of the "
           "user, two or three sentences, same language, adding nothing "
           "beyond the facts.";
  const ChatRequest req{
      .messages = {ChatMessage{.role = "system", .content = system},
                   ChatMessage{.role = "user", .content = base}},
      .maxTokens = 256,
      .temperature = 0.0F,
      .resetContext = true,
  };
  const std::string polished = llm_.chat(req);
  if (polished.empty())
    return;
  std::lock_guard<std::mutex> lock(profileMutex_);
  profileCache_ = {.userId = job.userId,
                   .lang = job.lang,
                   .text = polished,
                   .built = std::time(nullptr)};
}

void MemoryService::embedAndStore(int64_t factId, bool episode)
{
  if (factId <= 0)
    return;

  std::string scope;
  int64_t refId = 0;
  std::string content;
  {
    std::scoped_lock lock(vecDb_.mutex());
    sqlite3* db = vecDb_.handle();
    if (!db)
      return;
    const auto found = episode
                           ? graphRepo_.episodeContent(db, factId, scope, refId)
                           : graphRepo_.factContent(db, factId, scope, refId);
    if (!found)
      return;
    content = *found;
  }
  if (scope.empty() || content.empty())
    return;

  const int chunkChars = ConfigService::getInt("memory.chunk_chars");
  const int maxChars = chunkChars > 0 ? chunkChars : 400;
  const auto chunks = chunkContent(content, maxChars);
  const std::string expanded = episode ? "" : expandForEmbedding(content);
  const bool hasExpanded = !expanded.empty() && expanded != content;

  const auto primary = embedding_.embed(chunks.front(), "passage:");
  if (!primary)
    return;

  const std::string partition = memory_vec::partitionFor(scope, refId);

  std::scoped_lock lock(vecDb_.mutex());
  sqlite3* db = vecDb_.handle();
  if (!db)
    return;

  const std::string enc = memory_vec::encode(*primary);
  if (!episode) {
    const double dedupCfg = ConfigService::getDouble("memory.vector_dedup_sim");
    const float dedupFloor =
        dedupCfg > 0.0 ? static_cast<float>(dedupCfg) : 0.93F;
    const float dupSim = graphRepo_.vecDedupSim(db, enc, partition, factId);
    if (dupSim >= dedupFloor) {
      graphRepo_.bumpFactImportance(db, factId, std::time(nullptr));
      LOG_INFO << "MemoryService: fact " << factId
               << " merged as semantic duplicate (sim=" << dupSim
               << ", partition=" << partition << ")";
      return;
    }
    LOG_DEBUG << "MemoryService: fact " << factId
              << " nearest neighbour sim=" << dupSim;
  }
  graphRepo_.deleteVecRows(db, factId);

  int view = 0;
  for (const auto& chunk : chunks) {
    if (chunk == content) {
      graphRepo_.insertVecRow(db, partition, factId, view, *primary);
      ++view;
      continue;
    }
    const auto vec = embedding_.embed(chunk, "passage:");
    if (vec)
      graphRepo_.insertVecRow(db, partition, factId, view, *vec);
    ++view;
  }
  if (view == 0)
    graphRepo_.insertVecRow(db, partition, factId, 0, *primary);

  if (hasExpanded) {
    const auto vec = embedding_.embed(expanded, "passage:");
    if (vec)
      graphRepo_.insertVecRow(db, partition, factId, 100, *vec);
  }
}

void MemoryService::rebuildAll()
{
  std::vector<int64_t> ids;
  {
    std::scoped_lock lock(vecDb_.mutex());
    sqlite3* db = vecDb_.handle();
    if (!db)
      return;
    ids = graphRepo_.openFactIds(db);
  }
  LOG_INFO << "MemoryService: rebuilding " << ids.size() << " vectors";
  for (int64_t id : ids)
    embedAndStore(id, false);
}

void MemoryService::registerTools(ToolRegistry& registry)
{
  using tools::ToolArgumentSpec;
  registry.registerTool({.name = "memory.remember",
                         .description = "Almacena un hecho sobre una persona, "
                                        "dispositivo o lugar de la casa",
                         .arguments = {{"subject", "string", true, {}, ""},
                                       {"predicate", "string", true, {}, ""},
                                       {"value", "string", true, {}, ""},
                                       {"type",
                                        "enum",
                                        true,
                                        {"persona", "preference", "schedule",
                                         "instruction", "attribute"},
                                        ""},
                                       {"confidence", "number", false, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Create,
                         .handler = [this](const tools::ToolCall& call) {
                           return handleRemember(call);
                         }});
  registry.registerTool({.name = "memory.recall",
                         .description = "Recupera hechos guardados sobre la "
                                        "casa, las personas o los dispositivos",
                         .arguments = {{"query", "string", true, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Read,
                         .handler = [this](const tools::ToolCall& call) {
                           return handleRecall(call);
                         }});
  registry.registerTool({.name = "procedure.run",
                         .description = "Ejecuta un procedimiento conocido "
                                        "para conseguir un objetivo",
                         .arguments = {{"goal", "string", true, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Read,
                         .handler = [this](const tools::ToolCall& call) {
                           return handleProcedureRun(call);
                         }});
  registry.registerTool({.name = "memory.forget",
                         .description = "Olvida un hecho guardado por su id",
                         .arguments = {{"fact_id", "number", true, {}, ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Delete,
                         .handler = [this](const tools::ToolCall& call) {
                           return handleForget(call);
                         }});
}

int64_t MemoryService::observeSystemEvent(
    const std::string& channel, const std::string& summary,
    const std::string& actor, int64_t at, int64_t userId,
    const std::string& lang, const std::vector<int64_t>& entitiesHint)
{
  if (summary.empty())
    return 0;
  std::scoped_lock lock(graph_->mutex());
  return graph_->recordEpisode({.kind = channel,
                                .summary = summary,
                                .actor = actor,
                                .occurredAt = at,
                                .sessionId = {},
                                .lang = lang,
                                .scope = "user",
                                .refId = userId,
                                .salience = 0.5F,
                                .sourceId = std::nullopt,
                                .mentionEntityIds = entitiesHint});
}

int64_t MemoryService::recordProcedure(const std::string& name,
                                       const std::string& goal,
                                       const std::string& steps)
{
  std::scoped_lock lock(graph_->mutex());
  return graph_->recordProcedure(name, goal, steps);
}

tools::ToolResult MemoryService::handleProcedureRun(const tools::ToolCall& call)
{
  tools::ToolResult result;
  const std::string goal = call.arguments.get("goal", "").asString();
  std::optional<std::string> steps;
  {
    std::scoped_lock lock(graph_->mutex());
    steps = graph_->findProcedure(goal);
  }
  if (!steps || steps->empty()) {
    result.output = "no hay un procedimiento conocido para: " + goal;
    return result;
  }
  result.ok = true;
  result.output = *steps;
  return result;
}

tools::ToolResult MemoryService::handleRemember(const tools::ToolCall& call)
{
  tools::ToolResult result;
  const Json::Value& args = call.arguments;
  const std::string subject = args.get("subject", "").asString();
  const std::string predicate = args.get("predicate", "").asString();
  const std::string value = args.get("value", "").asString();
  const auto formed =
      formation_.observe({.channel = call.context.channel,
                          .text = subject + " " + predicate + " " + value,
                          .actor = "",
                          .at = std::time(nullptr),
                          .userId = call.context.userId,
                          .lang = call.context.lang,
                          .sessionId = call.context.sessionId,
                          .entitiesHint = {}},
                         call);
  if (!formed) {
    result.output = "no se pudo guardar el hecho";
    return result;
  }
  result.ok = true;
  result.data["fact_id"] = static_cast<int64_t>(formed->factId);
  result.data["subject_entity_id"] =
      static_cast<int64_t>(formed->subjectEntityId);
  result.output = "hecho guardado (id " + std::to_string(formed->factId) + ")";
  return result;
}

tools::ToolResult MemoryService::handleRecall(const tools::ToolCall& call)
{
  tools::ToolResult result;
  const std::string query = call.arguments.get("query", "").asString();
  const auto recalled = graphRecall_.recall({.text = query,
                                             .lang = call.context.lang,
                                             .scope = "user",
                                             .refId = call.context.userId,
                                             .maxHops = 1,
                                             .limit = 8,
                                             .addresseeEntityId = 0,
                                             .activeEntityIds = {}});
  if (recalled.hits.empty()) {
    result.output = "no hay recuerdos para esa consulta";
    return result;
  }
  result.ok = true;
  result.output = recalled.block;
  Json::Value& ids = result.data["fact_ids"] = Json::Value(Json::arrayValue);
  for (int64_t id : recalled.usedIds)
    ids.append(static_cast<int64_t>(id));
  bumpEpisodeHits(recalled.usedEpisodeIds);
  return result;
}

tools::ToolResult MemoryService::handleForget(const tools::ToolCall& call)
{
  tools::ToolResult result;
  const int64_t factId =
      static_cast<int64_t>(call.arguments.get("fact_id", 0).asInt64());
  if (factId <= 0) {
    result.output = "fact_id invalido";
    return result;
  }
  const bool ok = [&] {
    std::scoped_lock lock(graph_->mutex());
    return graph_->closeFact(factId, std::time(nullptr));
  }();
  if (!ok) {
    result.output = "el hecho no existe o ya estaba cerrado";
    return result;
  }
  result.ok = true;
  result.output = "hecho olvidado (id " + std::to_string(factId) + ")";
  return result;
}
