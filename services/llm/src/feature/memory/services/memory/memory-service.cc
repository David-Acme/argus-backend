#include "memory-service.hxx"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <drogon/drogon.h>
#include <iostream>
#include <mutex>
#include <set>
#include <string_view>
#include <config/config-service.hxx>
#include <feature/memory/services/embedding/embedding-service.hxx>
#include <feature/memory/services/extract/call-time.hxx>
#include <llm/llm-service.hxx>
#include <feature/memory/services/memory/memory-tool-descriptors.hxx>
#include <feature/memory/services/memory/memory-vec.hxx>
#include <phrase/rule-parser.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <sqlite/vec-db.hxx>
#include <text/text-norm.hxx>
#include <feature/memory/repositories/memory-graph/memory-graph-query.hxx>
#include <feature/memory/services/extract/vocabulary-lexicon.hxx>
#include <phrase/vocabulary.hxx>
#include <sqlite/sqlite-stmt.hxx>
#include <sqlite3.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

std::string memoryDbFile()
{
  std::string file = ConfigService::getString("memory.db_file");
  if (file.empty())
    file = ConfigService::getString("database.file");
  if (file.empty() || file.find("argus.db") != std::string::npos)
    file = "database/memory.db";
  return file;
}


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

}

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
  while (!stop_ && chat_.busy() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

void MemoryService::processExtract(const MemoryJob& job)
{
  if (job.preferIdle) {
    const int waitMs = ConfigService::getInt("memory.extract_wait_ms");
    waitForIdle(waitMs > 0 ? waitMs : 15000);
    if (chat_.busy()) {
      enqueueJob(job);
      return;
    }
  }
  const auto formed = formation_.observe({.channel = "user_turn",
                                          .text = job.text,
                                          .actor = {},
                                          .at = std::time(nullptr),
                                          .userId = job.userId,
                                          .lang = job.lang,
                                          .sessionId = {},
                                          .entitiesHint = {},
                                          .allowModel = true,
                                          .salient = job.salient,
                                          .decided = false,
                                          .typeHint = {},
                                          .refines = job.memoryId});
  if (!formed) {
    if (job.memoryId > 0)
      LOG_INFO << "MemoryService: note " << job.memoryId << " kept; the model found no fact in it";
    return;
  }
  LOG_INFO << "MemoryService: deferred extraction stored fact "
           << formed->factId << " (" << formed->source << ")"
           << (formed->refined ? " in place of note " + std::to_string(job.memoryId) : std::string());
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
    if (chat_.busy()) {
      enqueueJob(job);
      return;
    }
  }
  if (!chat_.available())
    return;

  const int maxTokens = ConfigService::getInt("memory.compact_max_tokens");
  const ChatRequest req{
      .messages = {ChatMessage{.role = "system", .content = kCompactSystem},
                   ChatMessage{.role = "user",
                               .content = "Conversation:\n" + job.text}},
      .maxTokens = maxTokens > 0 ? maxTokens : 256,
      .temperature = 0.0F,
      .resetContext = true,
      .stop = {},
      .grammar = {},
  };
  const std::string summary = chat_.chat(req);
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
    std::erase_if(queue_, [](const MemoryJob& job) {
      return job.kind == MemoryJob::Kind::Compact;
    });
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
    if (queue_.size() >= queueBound_) {
      if (job.kind != MemoryJob::Kind::Extract) {
        LOG_WARN << "MemoryService queue full (" << queue_.size()
                 << "); dropped " << static_cast<int>(job.kind) << " job";
        return;
      }
      const auto derived = std::find_if(
          queue_.begin(), queue_.end(), [](const MemoryJob& queued) {
            return queued.kind != MemoryJob::Kind::Extract;
          });
      if (derived == queue_.end()) {
        LOG_WARN << "MemoryService queue full of extracts; dropped one";
        return;
      }
      queue_.erase(derived);
    }
    queue_.push_back(std::move(job));
  }
  queueCv_.notify_one();
}

void MemoryService::init(const MemoryInitOptions& options)
{
  if (const int bound = ConfigService::getInt("memory.queue_bound"); bound > 0)
    queueBound_ = static_cast<size_t>(bound);
  phrases_.build();
  extractor_.rebuild(extract::allLexiconEntries());
  formation_.setExtractor(&extractor_);
  embedding_.init();
  if (options.deferStore) {
    drogon::app().registerBeginningAdvice([this]() { openStore(); });
    return;
  }
  openStore();
}

void MemoryService::openStore()
{
  {
    std::lock_guard<std::mutex> lock(storeMutex_);
    if (storeOpen_)
      return;
    storeOpen_ = true;
  }
  const std::string dbFile = memoryDbFile();
  graph_->open(dbFile);
  graph_->migrateLegacy();
  vecDb_.setDbFile(dbFile);
  vecDb_.applySchema(memory_graph_query::schemaFile());
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
  {
    std::lock_guard<std::mutex> lock(storeMutex_);
    storeOpen_ = false;
  }
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
                                          .salient = capture.salient,
                                          .decided = false,
                                          .typeHint = {},
                                          .refines = 0});
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
              .preferIdle = capture.preferIdle,
              .salient = capture.salient,
              .episode = false});
}

void MemoryService::refineLater(const NoteRefinement& refinement)
{
  if (refinement.note.source != "rule" || refinement.note.factId <= 0 || refinement.userId <= 0)
    return;
  enqueueJob({.kind = MemoryJob::Kind::Extract,
              .memoryId = refinement.note.factId,
              .userId = refinement.userId,
              .text = refinement.text,
              .lang = refinement.lang,
              .preferIdle = true,
              .salient = false,
              .episode = false});
}

bool MemoryService::hasUserScope(int64_t userId)
{
  return userId > 0;
}

CaptureResult MemoryService::captureExplicit(const CaptureInput& input)
{
  const RuleParseInput parsed{.text = input.text, .lang = input.lang};
  if (!hasUserScope(input.userId) || ruleParser_.isCancellation(parsed) ||
      ruleParser_.isVacuous(parsed))
    return {};

  if (!ruleParser_.parse(parsed)) {
    if (ruleParser_.isQuestion(parsed) || !ruleParser_.parseStatement(parsed))
      return {};
  }

  const InlineCapture capture{.channel = "user_turn",
                              .text = input.text,
                              .lang = input.lang,
                              .userId = input.userId,
                              .salient = false,
                              .preferIdle = false};
  const int64_t id = captureInline(capture);
  if (id > 0)
    return {.outcome = CaptureOutcome::Stored, .factId = id};

  deferCapture(capture);
  return {.outcome = CaptureOutcome::Deferred, .factId = 0};
}

CaptureResult MemoryService::captureImplicit(const CaptureInput& input)
{
  const RuleParseInput parsed{.text = input.text, .lang = input.lang};
  if (!hasUserScope(input.userId) || ruleParser_.isQuestion(parsed) ||
      ruleParser_.isCancellation(parsed) || ruleParser_.isVacuous(parsed))
    return {};

  deferCapture({.channel = "user_turn",
                .text = input.text,
                .lang = input.lang,
                .userId = input.userId,
                .salient = true,
                .preferIdle = true});
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

std::string MemoryService::durableTranscript(const std::string& transcript,
                                            const std::string& lang) const
{
  std::string durable;
  bool dropAnswer = false;
  size_t lineStart = 0;
  while (lineStart <= transcript.size()) {
    const size_t lineEnd = transcript.find('\n', lineStart);
    const std::string line =
        transcript.substr(lineStart, lineEnd == std::string::npos
                                         ? std::string::npos
                                         : lineEnd - lineStart);
    const bool isUser = line.rfind("user:", 0) == 0;
    const bool isAnswer = line.rfind("assistant:", 0) == 0;

    if (isUser) {
      const RuleParseInput parsed{.text = line.substr(5), .lang = lang};
      dropAnswer = ruleParser_.isQuestion(parsed) ||
                   ruleParser_.isCancellation(parsed) ||
                   ruleParser_.stripFillers(parsed).empty();
      if (!dropAnswer)
        durable += line + '\n';
    }
    else if (isAnswer) {
      if (!dropAnswer)
        durable += line + '\n';
      dropAnswer = false;
    }
    else if (!line.empty()) {
      durable += line + '\n';
    }

    if (lineEnd == std::string::npos)
      break;
    lineStart = lineEnd + 1;
  }
  return durable;
}

void MemoryService::enqueueSummary(const TranscriptJobInput& input)
{
  if (!running_ || input.userId < 0 || input.transcript.empty())
    return;
  if (!chat_.available())
    return;

  const std::string durable = durableTranscript(input.transcript, input.lang);
  if (durable.empty())
    return;

  enqueueJob({.kind = MemoryJob::Kind::Compact,
              .memoryId = 0,
              .userId = input.userId,
              .text = durable,
              .lang = input.lang,
              .preferIdle = false,
              .salient = false,
              .episode = false});
}

void MemoryService::enqueueCompaction(const TranscriptJobInput& input)
{
  if (!running_ || input.userId < 0 || input.transcript.empty())
    return;
  if (!chat_.available())
    return;

  const std::string durable = durableTranscript(input.transcript, input.lang);
  if (durable.empty())
    return;

  enqueueJob({.kind = MemoryJob::Kind::Compact,
              .memoryId = 0,
              .userId = input.userId,
              .text = durable,
              .lang = input.lang,
              .preferIdle = true,
              .salient = false,
              .episode = false});
}

bool MemoryService::flushPending(int timeoutMs)
{
  int budget = timeoutMs;
  if (budget <= 0)
    budget = ConfigService::getInt("memory.flush_timeout_ms");
  if (budget <= 0)
    budget = 60000;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(budget);
  while (std::chrono::steady_clock::now() < deadline) {
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (queue_.empty() && !working_.load())
        return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
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
  graphRepo_.bumpEpisodeHits(graph_->handle(),
                             {.ids = ids, .at = std::time(nullptr)});
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
  if (running_ && chat_.available()) {
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
    rows = graphRepo_.topProfileFacts(graph_->handle(), {.refId = userId, .limit = 6});
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
  if (chat_.busy()) {
    enqueueJob(job);
    return;
  }
  if (!chat_.available())
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
      .stop = {},
      .grammar = {},
  };
  const std::string polished = chat_.chat(req);
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
                           ? graphRepo_.episodeContent(db,
                                                       {.id = factId,
                                                        .scope = scope,
                                                        .refId = refId})
                           : graphRepo_.factContent(db,
                                                    {.id = factId,
                                                     .scope = scope,
                                                     .refId = refId});
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
    const float dupSim = graphRepo_.vecDedupSim(
        db, {.encoded = enc, .partition = partition, .factId = factId});
    if (dupSim >= dedupFloor) {
      graphRepo_.bumpFactImportance(db,
                                    {.factId = factId, .at = std::time(nullptr)});
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
      graphRepo_.insertVecRow(db,
                              {.partition = partition,
                               .factId = factId,
                               .view = view,
                               .vec = *primary});
      ++view;
      continue;
    }
    const auto vec = embedding_.embed(chunk, "passage:");
    if (vec)
      graphRepo_.insertVecRow(db,
                              {.partition = partition,
                               .factId = factId,
                               .view = view,
                               .vec = *vec});
    ++view;
  }
  if (view == 0)
    graphRepo_.insertVecRow(db,
                            {.partition = partition,
                             .factId = factId,
                             .view = 0,
                             .vec = *primary});

  if (hasExpanded) {
    const auto vec = embedding_.embed(expanded, "passage:");
    if (vec)
      graphRepo_.insertVecRow(db,
                              {.partition = partition,
                               .factId = factId,
                               .view = 100,
                               .vec = *vec});
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

std::vector<tools::ToolDescriptor> MemoryService::toolDescriptors()
{
  std::vector<tools::ToolDescriptor> descriptors = memoryToolDescriptors();
  for (auto& descriptor : descriptors) {
    if (descriptor.name == "memory.remember")
      descriptor.handler = [this](const tools::ToolCall& call) {
        return handleRemember(call);
      };
    else if (descriptor.name == "memory.remind")
      descriptor.handler = [this](const tools::ToolCall& call) {
        return handleRemind(call);
      };
    else if (descriptor.name == "memory.recall")
      descriptor.handler = [this](const tools::ToolCall& call) {
        return handleRecall(call);
      };
    else if (descriptor.name == "procedure.run")
      descriptor.handler = [this](const tools::ToolCall& call) {
        return handleProcedureRun(call);
      };
    else if (descriptor.name == "memory.forget")
      descriptor.handler = [this](const tools::ToolCall& call) {
        return handleForget(call);
      };
  }
  return descriptors;
}

int64_t MemoryService::observeSystemEvent(const SystemEventInput& input)
{
  if (input.summary.empty() || !hasUserScope(input.userId))
    return 0;
  std::scoped_lock lock(graph_->mutex());
  return graph_->recordEpisode({.kind = input.channel,
                                .summary = input.summary,
                                .actor = input.actor,
                                .occurredAt = input.at,
                                .sessionId = {},
                                .lang = input.lang,
                                .scope = "user",
                                .refId = input.userId,
                                .salience = 0.5F,
                                .sourceId = std::nullopt,
                                .mentionEntityIds = input.entitiesHint});
}

int64_t MemoryService::recordProcedure(const ProcedureRecordInput& input)
{
  std::scoped_lock lock(graph_->mutex());
  return graph_->recordProcedure(input);
}

namespace
{

bool english(const tools::ToolCall& call)
{
  return call.context.lang == "en";
}

std::string scopeRefusal(const tools::ToolCall& call)
{
  return english(call) ? "Memory is only available to identified users."
                       : "La memoria solo está disponible para usuarios identificados.";
}

std::vector<std::string> contentWords(const std::string& text)
{
  return text_norm::words(text_norm::stripAccents(text), 3);
}

struct GroundingInput
{
  const std::string& candidate;
  const std::string& utterance;
};

bool groundedIn(const GroundingInput& input)
{
  const auto words = contentWords(input.candidate);
  if (words.empty())
    return false;
  const auto heard = text_norm::wordSet(text_norm::stripAccents(input.utterance), 3);
  const auto found = std::ranges::count_if(
      words, [&heard](const std::string& word) { return heard.contains(word); });
  return found * 5 >= static_cast<std::ptrdiff_t>(words.size()) * 3;
}

struct OverlapInput
{
  const std::string& query;
  const std::string& canonical;
};

std::ptrdiff_t sharedWords(const OverlapInput& input)
{
  const auto heard = text_norm::wordSet(text_norm::stripAccents(input.canonical), 4);
  return std::ranges::count_if(contentWords(input.query), [&heard](const std::string& word) {
    return word.size() >= 4 && heard.contains(word);
  });
}

tools::ToolCall groundedCall(const tools::ToolCall& call)
{
  tools::ToolCall grounded = call;
  const std::string& utterance = call.context.utterance;
  if (utterance.empty() || !grounded.arguments.isObject())
    return grounded;
  const std::string text = grounded.arguments.get("text", "").asString();
  if (text.empty() || !groundedIn({.candidate = text, .utterance = utterance}))
    grounded.arguments["text"] = utterance;
  const std::string value = grounded.arguments.get("value", "").asString();
  if (!value.empty() && !groundedIn({.candidate = value, .utterance = utterance})) {
    grounded.arguments.removeMember("subject");
    grounded.arguments.removeMember("predicate");
    grounded.arguments.removeMember("value");
  }
  return grounded;
}

std::string spoken(std::string text)
{
  while (!text.empty() && std::string_view(" .,;:!?").find(text.back()) != std::string_view::npos)
    text.pop_back();
  return text + ".";
}

int recallLimit()
{
  const int topK = ConfigService::getInt("memory.recall_top_k");
  return topK > 0 ? topK : 4;
}

}

tools::ToolResult MemoryService::handleProcedureRun(const tools::ToolCall& call)
{
  tools::ToolResult result;
  if (!hasUserScope(call.context.userId)) {
    result.output = scopeRefusal(call);
    return result;
  }
  const std::string goal = call.arguments.get("goal", "").asString();
  std::optional<std::string> steps;
  {
    std::scoped_lock lock(graph_->mutex());
    steps = graph_->findProcedure(goal);
  }
  if (!steps || steps->empty()) {
    result.output = (english(call) ? "There is no known procedure for: "
                                   : "No hay un procedimiento conocido para: ") +
                    goal;
    return result;
  }
  result.ok = true;
  result.output = *steps;
  return result;
}

tools::ToolResult MemoryService::handleRemember(const tools::ToolCall& call)
{
  tools::ToolResult result;
  if (!hasUserScope(call.context.userId)) {
    result.output = scopeRefusal(call);
    return result;
  }
  const tools::ToolCall grounded = groundedCall(call);
  const Json::Value& args = grounded.arguments;
  const std::string subject = args.get("subject", "").asString();
  const std::string predicate = args.get("predicate", "").asString();
  const std::string value = args.get("value", "").asString();
  std::string text = args.get("text", "").asString();
  if (text.empty())
    text = call.context.utterance;
  if (text.empty())
    text = subject + " " + predicate + " " + value;
  const auto observe = [&](const std::string& candidate) {
    return formation_.observe({.channel = call.context.channel,
                               .text = candidate,
                               .actor = "",
                               .at = std::time(nullptr),
                               .userId = call.context.userId,
                               .lang = call.context.lang,
                               .sessionId = call.context.sessionId,
                               .entitiesHint = {},
                               .allowModel = false,
                               .salient = false,
                               .decided = call.context.decided,
                               .typeHint = {},
                               .refines = 0},
                              grounded);
  };
  auto formed = observe(text);
  if (!formed && !call.context.utterance.empty() &&
      call.context.utterance != text) {
    text = call.context.utterance;
    formed = observe(text);
  }
  if (!formed) {
    result.output = english(call) ? "I could not save that." : "No pude guardar eso.";
    return result;
  }
  result.ok = true;
  result.data["fact_id"] = static_cast<int64_t>(formed->factId);
  result.data["subject_entity_id"] =
      static_cast<int64_t>(formed->subjectEntityId);
  enqueueJob({.kind = MemoryJob::Kind::Embed,
              .memoryId = formed->factId,
              .userId = 0,
              .text = {},
              .lang = {},
              .preferIdle = false,
              .salient = false,
              .episode = false});
  refineLater({.note = *formed, .userId = call.context.userId, .text = text, .lang = call.context.lang});
  result.output = (english(call) ? "Saved: " : "Guardado: ") + spoken(formed->canonical);
  return result;
}

tools::ToolResult MemoryService::handleRemind(const tools::ToolCall& call)
{
  tools::ToolResult result;
  if (!hasUserScope(call.context.userId)) {
    result.output = scopeRefusal(call);
    return result;
  }
  const tools::ToolCall grounded = groundedCall(call);
  std::string text = grounded.arguments.get("text", "").asString();
  if (text.empty())
    text = call.context.utterance;
  if (text.empty()) {
    result.output = english(call) ? "There is nothing to remind." : "No hay nada que recordar.";
    return result;
  }

  const auto formed = formation_.observe({.channel = call.context.channel,
                                          .text = text,
                                          .actor = "",
                                          .at = std::time(nullptr),
                                          .userId = call.context.userId,
                                          .lang = call.context.lang,
                                          .sessionId = call.context.sessionId,
                                          .entitiesHint = {},
                                          .allowModel = false,
                                          .salient = false,
                                          .decided = call.context.decided,
                                          .typeHint = "schedule",
                                          .refines = 0},
                                         grounded);
  if (!formed) {
    result.output = english(call) ? "I could not save the reminder."
                                  : "No pude guardar el recordatorio.";
    return result;
  }

  result.ok = true;
  result.data["fact_id"] = static_cast<int64_t>(formed->factId);
  result.data["subject_entity_id"] =
      static_cast<int64_t>(formed->subjectEntityId);
  enqueueJob({.kind = MemoryJob::Kind::Embed,
              .memoryId = formed->factId,
              .userId = 0,
              .text = {},
              .lang = {},
              .preferIdle = false,
              .salient = false,
              .episode = false});
  result.output = (english(call) ? "Reminder saved: " : "Recordatorio guardado: ") +
                  spoken(formed->canonical);
  if (const auto when = scheduleReminderCall({.call = call,
                                              .text = text,
                                              .factId = formed->factId}))
    result.output += (english(call) ? " I will call you at " : " Te llamaré a las ") +
                     *when + ".";
  return result;
}

std::optional<std::string>
MemoryService::scheduleReminderCall(const ReminderCallInput& input) const
{
  if (!reminderCalls_ || input.call.context.userId <= 0)
    return std::nullopt;
  const auto now = static_cast<int64_t>(std::time(nullptr));
  const std::string& utterance = input.call.context.utterance.empty()
                                     ? input.text
                                     : input.call.context.utterance;
  const auto when = call_time::resolve(
      {.text = utterance, .lang = input.call.context.lang, .now = now});
  if (!when)
    return std::nullopt;
  std::string topic = input.text;
  if (const auto inText = call_time::resolve(
          {.text = input.text, .lang = input.call.context.lang, .now = now}))
    topic = call_time::withoutPhrase(input.text, *inText);
  if (topic.empty())
    topic = input.text;
  const bool scheduled = reminderCalls_->schedule(
      {.userId = input.call.context.userId,
       .fireAt = when->fireAt,
       .topic = topic,
       .lang = input.call.context.lang,
       .commandId = "memory-remind:" + std::to_string(input.factId) + ":" +
                    std::to_string(when->fireAt)});
  if (!scheduled)
    return std::nullopt;
  const auto seconds = static_cast<std::time_t>(when->fireAt);
  std::tm local{};
  localtime_r(&seconds, &local);
  std::array<char, 8> clock{};
  const std::size_t written =
      std::strftime(clock.data(), clock.size(), "%H:%M", &local);
  return std::string(clock.data(), written);
}

void MemoryService::setReminderCalls(
    std::shared_ptr<const ReminderCallScheduler> scheduler)
{
  reminderCalls_ = std::move(scheduler);
}

tools::ToolResult MemoryService::handleRecall(const tools::ToolCall& call)
{
  tools::ToolResult result;
  if (!hasUserScope(call.context.userId)) {
    result.output = scopeRefusal(call);
    return result;
  }
  const std::string query = call.arguments.get("query", "").asString();
  const auto recalled = graphRecall_.recall({.text = query,
                                             .lang = call.context.lang,
                                             .scope = "user",
                                             .refId = call.context.userId,
                                             .maxHops = 1,
                                             .limit = recallLimit(),
                                             .addresseeEntityId = 0,
                                             .activeEntityIds = {}});
  if (recalled.hits.empty()) {
    result.output = english(call) ? "I have nothing saved about that."
                                  : "No tengo nada guardado sobre eso.";
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
  if (!hasUserScope(call.context.userId)) {
    result.output = scopeRefusal(call);
    return result;
  }
  const std::string query = call.arguments.get("query", "").asString();
  const auto recalled = graphRecall_.recall({.text = query,
                                             .lang = call.context.lang,
                                             .scope = "user",
                                             .refId = call.context.userId,
                                             .maxHops = 1,
                                             .limit = 3,
                                             .addresseeEntityId = 0,
                                             .activeEntityIds = {}});
  auto target = recalled.hits.end();
  std::ptrdiff_t best = 0;
  for (auto hit = recalled.hits.begin(); hit != recalled.hits.end(); ++hit) {
    const std::ptrdiff_t shared =
        hit->factId > 0 ? sharedWords({.query = query, .canonical = hit->canonical}) : 0;
    if (shared > best) {
      best = shared;
      target = hit;
    }
  }
  const bool closed = target != recalled.hits.end() && [&] {
    std::scoped_lock lock(graph_->mutex());
    return graph_->closeFact({.factId = target->factId,
                              .refId = call.context.userId,
                              .at = std::time(nullptr)});
  }();
  if (!closed) {
    result.output = english(call) ? "I found no such memory." : "No encontré ese recuerdo.";
    return result;
  }
  result.ok = true;
  result.data["fact_id"] = static_cast<int64_t>(target->factId);
  result.output = (english(call) ? "Forgotten: " : "Olvidado: ") + spoken(target->canonical);
  return result;
}
