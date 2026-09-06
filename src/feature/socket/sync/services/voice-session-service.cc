#include "voice-session-service.hxx"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <drogon/drogon.h>
#include <shared/contracts/sync-operation.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/tts/onnx-utils.hxx>

namespace
{

constexpr int kTargetRate = 16000;

// Root-mean-square of a float sample buffer (16 kHz, [-1, 1]).
float rmsOf(const std::vector<float>& samples)
{
  if (samples.empty())
    return 0.0F;
  double sum = 0.0;
  for (const float s : samples)
    sum += static_cast<double>(s) * s;
  return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}
// Minimum sentence length (chars) before a `. ! ?` boundary flushes mid-
// stream. The first sentence flushes at any boundary; shorter sentences are
// held until the stream ends so tiny fragments are not spoken separately.
constexpr size_t kMinSentenceChars = 10;
// Clause boundaries (`, ; :`) only split after this much text, and only when
// enough text follows — a small look-ahead keeps prosody intact.
constexpr size_t kClauseMinChars = 46;
constexpr size_t kClauseTailChars = 22;
// Hard cap: cut run-on answers at the last space (never break a word).
constexpr size_t kHardMaxChars = 110;
constexpr size_t kHardMinCut = 28;

// Opening lines per language. Extend this map when a new VoiceLang is
// supported end to end. `{name}` is replaced with the user's name when
// known, otherwise the name-asking variant is spoken.
struct Greeting
{
  std::string withName; // spoken when the user's name is known
  std::string askName;  // spoken when the name is unknown
};

const std::unordered_map<VoiceLang, Greeting>& greetings()
{
  static const std::unordered_map<VoiceLang, Greeting> map = {
      {VoiceLang::Es,
       {"Hola {name}, soy Argus, tu asistente. ¿En qué puedo ayudarte?",
        "Hola, soy Argus, tu asistente local. ¿Cómo te llamas?"}},
      {VoiceLang::En,
       {"Hi {name}, I'm Argus, your assistant. How can I help you?",
        "Hi, I'm Argus, your local assistant. What's your name?"}},
  };
  return map;
}

std::string greetingFor(VoiceLang lang, const std::string& name)
{
  const auto it = greetings().find(lang);
  if (it == greetings().end())
    return lang == VoiceLang::En ? "Hi, I'm Argus, your local assistant."
                                 : "Hola, soy Argus, tu asistente local.";
  const bool known = name.size() >= 2;
  std::string text = known ? it->second.withName : it->second.askName;
  if (known) {
    const auto pos = text.find("{name}");
    if (pos != std::string::npos)
      text.replace(pos, 7, name);
  }
  return text;
}

std::string langDisplayName(VoiceLang lang)
{
  switch (lang) {
    case VoiceLang::En:
      return "English";
    case VoiceLang::Es:
      return "Spanish";
    case VoiceLang::System:
      break;
  }
  return "Spanish";
}

// RAII: `speaking` must never stay true. If TTS synthesis throws, the
// session would otherwise drop every mic frame forever ("stops hearing
// you").
class SpeakingGuard
{
public:
  explicit SpeakingGuard(std::atomic<bool>& flag) : flag_(flag)
  {
    flag_.store(true);
  }
  ~SpeakingGuard() { flag_.store(false); }

  SpeakingGuard(const SpeakingGuard&) = delete;
  SpeakingGuard& operator=(const SpeakingGuard&) = delete;

private:
  std::atomic<bool>& flag_;
};

std::string systemPrompt(VoiceLang lang)
{
  const std::string langName = langDisplayName(lang);
  std::string prompt =
      "You are Argus, a warm, natural home voice assistant for a local "
      "security camera system.\n"
      "Guidelines:\n"
      "- Reply strictly in " +
      langName +
      ". Never switch to another language.\n"
      "- Speak like a person, not a help desk: short, warm and direct.\n"
      "- Address the user informally (\"tú\" in Spanish), never \"usted\".\n"
      "- Never speak as if you were the user: the user's facts are yours to "
      "describe, not to own.\n"
      "- Engage with what the user just said; never answer with generic "
      "offers such as \"how can I help you\".\n"
      "- Answer in at most two short sentences and stop there.\n"
      "- End every sentence with a period, question mark or exclamation "
      "mark; split long ideas into several short sentences so the reply "
      "sounds like natural speech when spoken aloud.\n"
      "- Your reply is spoken aloud: natural sentences, no lists, no "
      "symbols or abbreviations.\n"
      "- If you do not know something, say so honestly; do not invent.\n"
      "- If you do not know the user's name, ask for it once, naturally.\n";
  return prompt;
}

std::string stripPrefix(const std::string& text)
{
  std::string out = text;
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front())))
    out.erase(out.begin());
  std::string lower = out;
  for (auto& c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (std::string_view p : {"argus:", "argus "}) {
    if (lower.compare(0, p.size(), p) == 0) {
      out.erase(0, p.size());
      while (!out.empty() &&
             (std::isspace(static_cast<unsigned char>(out.front())) ||
              out.front() == ':'))
        out.erase(out.begin());
      break;
    }
  }
  return out;
}

// Streaming chunk boundary for TTS. Returns the byte length of the next
// chunk to speak (0 = keep buffering):
//  - `. ! ?` followed by whitespace (or end of text) → flush the sentence.
//  - `, ; :` after a minimum length with a look-ahead tail → flush the
//    clause so the reply paces like human speech instead of one gulp.
//  - run-on text past the hard cap → cut at the last space.
size_t nextChunkEnd(const std::string& text, bool firstSentence)
{
  const size_t len = text.size();
  for (size_t i = 0; i < len; ++i) {
    const char c = text[i];
    if (c == '.' || c == '!' || c == '?') {
      size_t j = i + 1;
      while (j < len && (text[j] == '.' || text[j] == '!' ||
                         text[j] == '?' || text[j] == '"' ||
                         text[j] == '\''))
        ++j;
      if (j == len)
        return len; // sentence complete at the end of the stream
      if (!std::isspace(static_cast<unsigned char>(text[j])))
        continue; // "3.5", "Dr. Smith" — not a boundary
      if (firstSentence || i >= kMinSentenceChars)
        return j;
    }
    if ((c == ',' || c == ';' || c == ':') &&
        i >= kClauseMinChars &&
        i + 1 < len &&
        std::isspace(static_cast<unsigned char>(text[i + 1])) &&
        len - i - 1 >= kClauseTailChars) {
      return i + 1; // cut right after the clause mark
    }
  }
  if (len >= kHardMaxChars) {
    const size_t lastSpace = text.rfind(' ');
    if (lastSpace != std::string::npos && lastSpace >= kHardMinCut)
      return lastSpace;
  }
  return 0;
}

// Extracts a name from "me llamo X" / "soy X" / "mi nombre es X" replies.
std::optional<std::string> extractName(const std::string& text)
{
  std::string lower = text;
  for (auto& c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  for (const std::string_view pattern : {"mi nombre es ", "me llamo ",
                                         "yo soy ", "soy "}) {
    const auto pos = lower.find(pattern);
    if (pos == std::string::npos)
      continue;
    std::string name = text.substr(pos + pattern.size());
    while (!name.empty() &&
           std::isspace(static_cast<unsigned char>(name.front())))
      name.erase(name.begin());
    const auto end = std::find_if(name.begin(), name.end(), [](unsigned char c) {
      return c == '.' || c == ',' || c == '!' || c == '?';
    });
    name = std::string(name.begin(), end);
    while (!name.empty() &&
           std::isspace(static_cast<unsigned char>(name.back())))
      name.pop_back();
    if (name.size() >= 2 && name.size() <= 40) {
      name[0] = static_cast<char>(
          std::toupper(static_cast<unsigned char>(name[0])));
      return name;
    }
  }
  return std::nullopt;
}

std::vector<int16_t> floatToInt16(const std::vector<float>& pcm)
{
  std::vector<int16_t> out;
  out.reserve(pcm.size());
  for (const float v : pcm) {
    const float clamped = std::max(-1.0F, std::min(1.0F, v));
    out.push_back(static_cast<int16_t>(clamped * 32767.0F));
  }
  return out;
}

} // namespace

VoiceLang voiceSystemLang()
{
  return voiceLangFromString(ConfigService::getString("stt.language"));
}

VoiceSessionService::VoiceSessionService(const VoiceEngineSeam& engines)
    : stt_(engines.stt), tts_(engines.tts), llm_(engines.llm)
{
  reactions_.init();
}

Reaction VoiceSessionService::emitReaction(Session& session,
                                          const ReactionSignals& signals)
{
  const Reaction reaction = reactions_.react(signals);
  Json::Value payload;
  payload["reaction"] = reactionKindToString(reaction.kind);
  payload["intensity"] = reaction.intensity;
  payload["because"] = reaction.because;
  sendJson(session, "voice:event", payload);
  return reaction;
}

void VoiceSessionService::start(const drogon::WebSocketConnectionPtr& conn,
                                int64_t userId, VoiceLang lang,
                                const std::string& userName)
{
  if (lang == VoiceLang::System)
    lang = voiceSystemLang();

  LOG_INFO << "Voice: session start user=" << userId
           << " lang=" << voiceLangToString(lang)
           << " nameKnown=" << (userName.size() >= 2);

  auto session = std::make_shared<Session>();
  session->conn = conn;
  session->lang = lang;
  session->userId = userId;
  session->nameKnown = userName.size() >= 2;
  session->denoise = ConfigService::getBool("vad.denoise");
  const double gateRms = ConfigService::getDouble("vad.denoise_gate_rms");
  session->denoiseGateRms = gateRms > 0.0 ? static_cast<float>(gateRms) : 0.0035F;
  session->history.push_back({"system", systemPrompt(session->lang)});

  // The shared recognizer follows the session language (falls back to the
  // system default when the recognizer cannot switch).
  const std::string code = voiceLangToString(session->lang);
  if (!code.empty())
    stt_.setLanguage(code);

  const std::string greeting = greetingFor(session->lang, userName);
  session->history.push_back({"assistant", greeting});

  session->worker = std::thread([this, session, greeting] {
    speak(*session, greeting);
    workerLoop(session);
  });

  std::lock_guard<std::mutex> lock(mutex_);
  sessions_[conn.get()] = std::move(session);
}

void VoiceSessionService::feedPcm(const drogon::WebSocketConnectionPtr& conn,
                                  const char* data, size_t len)
{
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(conn.get());
    if (it == sessions_.end())
      return;
    session = it->second;
  }

  // While the assistant speaks, the app mic may pick up its own audio (echo);
  // drop those frames instead of feeding them to the VAD.
  if (session->speaking)
    return;

  const size_t sampleCount = len / 2;
  std::vector<float> floats(sampleCount);
  for (size_t i = 0; i < sampleCount; ++i) {
    const int16_t s = static_cast<int16_t>(
        (static_cast<unsigned char>(data[i * 2]) |
         (static_cast<unsigned char>(data[i * 2 + 1]) << 8)));
    floats[i] = static_cast<float>(s) / 32768.0F;
  }

  {
    std::lock_guard<std::mutex> lock(session->pcmMutex);
    session->pcmQueue.insert(session->pcmQueue.end(), floats.begin(),
                             floats.end());
  }
  session->pcmCv.notify_one();
}

void VoiceSessionService::stop(const drogon::WebSocketConnectionPtr& conn)
{
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(conn.get());
    if (it == sessions_.end())
      return;
    session = it->second;
    sessions_.erase(it);
  }
  session->active.store(false);
  session->pcmCv.notify_all();
  if (session->worker.joinable())
    session->worker.join();

  if (session->conn && !session->conn->disconnected()) {
    Json::Value done;
    done["sessionId"] = 0;
    sendJson(*session, "voice:done", done);
  }
}

void VoiceSessionService::workerLoop(std::shared_ptr<Session> session)
{
  while (session->active.load()) {
    std::vector<float> batch;
    {
      std::unique_lock<std::mutex> lock(session->pcmMutex);
      session->pcmCv.wait(lock, [&] {
        return !session->pcmQueue.empty() || !session->active.load();
      });
      if (!session->active.load())
        break;
      batch.swap(session->pcmQueue);
    }

    // While the assistant speaks, the mic may pick up its own audio; drop
    // those samples instead of feeding them to the VAD.
    if (session->speaking)
      continue;

    // RNNoise strips background noise first, so the VAD only reacts to the
    // speaker's voice (and the STT never sees raw noisy audio). The AGC +
    // voice-probability blend keeps quiet speech alive. Batches below the
    // silence gate (no signal, no recently-decaying voice) skip the whole
    // RNNoise path — that is where most CPU goes when nobody is talking.
    std::vector<float> clean;
    if (session->denoise) {
      const bool silent = rmsOf(batch) < session->denoiseGateRms &&
                          !session->denoiser.recentVoice();
      if (silent) {
        clean.swap(batch);
      } else {
        session->denoiser.process(batch, clean);
      }
      if ((++session->denoiseLogCounter % 25) == 0)
        LOG_INFO << "Voice: denoise prob=" << session->denoiser.lastVoiceProb();
    } else {
      clean.swap(batch);
    }

    // Feed the VAD window by window. The VAD keeps its own state across
    // batches; when it completes a turn (speech + trailing silence), run
    // STT → LLM → TTS. Audio that did not complete a turn stays buffered
    // inside the VAD for the next batch.
    size_t offset = 0;
    while (offset < clean.size() && session->active.load()) {
      const size_t chunk = std::min<size_t>(512, clean.size() - offset);
      VadTurn turn;
      if (session->vad.process(clean.data() + offset,
                               static_cast<int>(chunk), turn)) {
        if (!turn.samples.empty()) {
          try {
            processTurn(*session, turn.samples);
          }
          catch (const std::exception& e) {
            // Last line of defence: a turn must never kill the worker.
            LOG_WARN << "Voice: turn failed: " << e.what();
            session->speaking.store(false);
          }
          catch (...) {
            LOG_WARN << "Voice: turn failed (unknown error)";
            session->speaking.store(false);
          }
        }
      }
      offset += chunk;
    }
  }
}

void VoiceSessionService::processTurn(Session& session,
                                      const std::vector<float>& samples)
{
  // A new turn starts clean: a previous skip() must not leak into it.
  session.interrupt.store(false);

  std::string userText;
  try {
    userText = stt_.transcribe(samples, 16000);
  }
  catch (const std::exception& e) {
    // A broken recognizer must not kill the worker (std::terminate on an
    // uncaught exception in a std::thread).
    LOG_WARN << "Voice: STT failed: " << e.what();
    emitReaction(session, {.text = {},
                           .lang = session.lang == VoiceLang::En ? "en" : "es",
                           .captureStored = false,
                           .captureQueued = false,
                           .recallHits = -1,
                           .cameraIntent = false,
                           .sttFailed = true,
                           .systemAlert = false});
    return;
  }
  if (userText.empty()) {
    LOG_WARN << "Voice: STT returned empty for a VAD turn";
    emitReaction(session, {.text = {},
                           .lang = session.lang == VoiceLang::En ? "en" : "es",
                           .captureStored = false,
                           .captureQueued = false,
                           .recallHits = -1,
                           .cameraIntent = false,
                           .sttFailed = true,
                           .systemAlert = false});
    return;
  }
  LOG_INFO << "Voice: STT -> " << userText;

  Json::Value stt;
  stt["text"] = userText;
  stt["final"] = true;
  sendJson(session, "voice:stt", stt);

  session.history.push_back({"user", userText});

  // Persist the user's name when given (onboarding). The repository methods
  // are coroutines, so the update runs on the event loop.
  if (session.userId > 0 && !session.nameKnown) {
    if (const auto name = extractName(userText)) {
      const std::string persisted = *name;
      const int64_t userId = session.userId;
      drogon::app().getLoop()->queueInLoop([userId, persisted]() {
        drogon::async_run([userId, persisted]() -> drogon::Task<void> {
          auto user = co_await UserRepository().update(
              userId,
              {.name = persisted,
               .lastName = std::nullopt,
               .role = std::nullopt,
               .isActive = std::nullopt});
          if (user.id > 0) {
            SocketEmitDto emit;
            emit.operation = SyncOperation::Add;
            emit.option = TableName::User;
            emit.obj = user.toJson();
            SocketService().emitModule(TableName::User, emit);
          }
          co_return;
        });
      });
      session.nameKnown = true;
    }
  }

  // The reaction goes out BEFORE generating: the face reacts while Argus
  // thinks instead of after he speaks.
  const Reaction reaction =
      emitReaction(session, {.text = userText,
                             .lang = session.lang == VoiceLang::En ? "en" : "es",
                             .captureStored = false,
                             .captureQueued = false,
                             .recallHits = -1,
                             .cameraIntent = false,
                             .sttFailed = false,
                             .systemAlert = false});

  ChatRequest req;
  req.messages = session.history;
  // The tone rides the tail of the request copy, never the system prompt (the
  // prefix has to stay constant for KV reuse) and never the stored history
  // (a note from three turns ago must not keep steering the answer).
  const std::string tone = ReactionEngine::toneNote(
      reaction, session.lang == VoiceLang::En ? "en" : "es");
  if (!tone.empty() && !req.messages.empty())
    req.messages.back().content += tone;

  std::string full;
  std::string pending;
  bool prefixStripped = false;
  bool firstSentenceSent = false;

  try {
    llm_.chatStream(req, [&](const std::string& token, bool) {
      if (!session.conn || session.conn->disconnected())
        return;
      if (session.interrupt.load())
        return; // skip(): swallow the rest of the stream

      // Tokens are appended verbatim: llama tokenization puts the space at
      // the START of the next word's token, so stripping per token glues
      // words together ("Con gusto" -> "Congusto"). Only the model's
      // "Argus:" preamble is stripped once, from the accumulated buffer.
      full += token;
      pending += token;
      if (!prefixStripped && pending.size() >= 8) {
        pending = stripPrefix(pending);
        prefixStripped = true;
      }

      for (;;) {
        const size_t cut = nextChunkEnd(pending, !firstSentenceSent);
        if (cut == 0)
          break;
        const std::string sentence = pending.substr(0, cut);
        pending.erase(0, cut);
        firstSentenceSent = true;
        speak(session, sentence);
        if (session.interrupt.load()) {
          pending.clear();
          break;
        }
      }
    });
  }
  catch (const std::exception& e) {
    // Same rule as TTS: a failing LLM must not kill the worker.
    LOG_WARN << "Voice: LLM failed: " << e.what();
  }
  catch (...) {
    LOG_WARN << "Voice: LLM failed (unknown error)";
  }

  if (!pending.empty() && !session.interrupt.load())
    speak(session, pending);

  if (!full.empty())
    session.history.push_back({"assistant", stripPrefix(full)});
  if (session.history.size() > 21) {
    session.history.erase(session.history.begin() + 1,
                          session.history.begin() + 3);
  }
  session.speaking = false;

  // Reset the VAD so the next turn starts from a clean slate. The PCM queue
  // is NOT cleared: audio captured while the LLM was "thinking" (the mic
  // only pauses at the first TTS chunk) may hold a real interjection, and
  // the worker processes it as the next turn.
  session.vad.reset();
}

void VoiceSessionService::speak(Session& session, const std::string& text)
{
  if (text.empty() || !session.conn || session.conn->disconnected())
    return;
  LOG_INFO << "Voice: speaking -> " << text.substr(0, 80);

  TtsRequest treq;
  treq.text = text;
  treq.lang = session.lang == VoiceLang::En ? TtsLang::EN : TtsLang::ES;
  treq.quality = TtsQuality::Auto;
  // defaultSpeed/sampleRate reach over the argus-tts wire since the F4-2
  // cutover; a refused connection degrades to neutral synthesis parameters
  // (the synthesis below reports the same failure).
  int ttsRate = kTargetRate;
  try {
    treq.speed = tts_.defaultSpeed();
    ttsRate = tts_.sampleRate();
  }
  catch (const std::exception& e) {
    LOG_WARN << "Voice: TTS unavailable: " << e.what();
  }
  AudioResampler resampler(
      {.sourceRate = ttsRate > 0 ? ttsRate : kTargetRate,
       .targetRate = kTargetRate});

  SpeakingGuard speakingGuard(session.speaking);
  Json::Value assistant;
  assistant["text"] = text;
  try {
    tts_.synthesizeStream(treq, [&](const std::vector<float>& chunk) {
      if (!session.conn || session.conn->disconnected())
        return;
      if (session.interrupt.load())
        return; // skip(): cut the audio; the text is still reported below
      const auto raw = floatToInt16(chunk);
      std::vector<int16_t> resampled;
      resampler.process(raw.data(), raw.size(), resampled);
      if (!resampled.empty()) {
        session.conn->send(
            std::string_view(reinterpret_cast<const char*>(resampled.data()),
                             resampled.size() * sizeof(int16_t)),
            drogon::WebSocketMessageType::Binary);
      }
    });
  }
  catch (const std::exception& e) {
    // A TTS failure must not kill the worker thread nor leave the session
    // permanently deaf (SpeakingGuard handles the latter).
    LOG_WARN << "Voice: TTS failed: " << e.what();
  }
  catch (...) {
    LOG_WARN << "Voice: TTS failed (unknown error)";
  }
  sendJson(session, "voice:assistant", assistant);
}

void VoiceSessionService::skip(const drogon::WebSocketConnectionPtr& conn)
{
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(conn.get());
    if (it == sessions_.end())
      return;
    session = it->second;
  }
  LOG_INFO << "Voice: skip";
  session->interrupt.store(true);
}

void VoiceSessionService::sendJson(Session& session, const std::string& type,
                                   const Json::Value& payload) const
{
  if (!session.conn || session.conn->disconnected())
    return;
  Json::Value msg;
  msg["type"] = type;
  msg["payload"] = payload;
  session.conn->sendJson(msg);
}
