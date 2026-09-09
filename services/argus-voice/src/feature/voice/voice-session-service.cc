#include "voice-session-service.hxx"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>

namespace
{

constexpr int kTargetRate = 16000;

// Root-mean-square of a float sample buffer.
float rmsOf(const std::vector<float>& samples)
{
  if (samples.empty())
    return 0.0F;
  double sum = 0.0;
  for (const float s : samples)
    sum += static_cast<double>(s) * s;
  return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

// Sentence flush threshold (chars).
constexpr size_t kMinSentenceChars = 10;
// Clause split thresholds (chars).
constexpr size_t kClauseMinChars = 46;
constexpr size_t kClauseTailChars = 22;
// Hard-cap thresholds for run-on text (chars).
constexpr size_t kHardMaxChars = 110;
constexpr size_t kHardMinCut = 28;

// Opening lines per language; {name} is substituted when known.
struct Greeting
{
  std::string withName;
  std::string askName;
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

VoiceLang voiceLangFromProto(argus::voice::v1::VoiceLanguage lang)
{
  switch (lang) {
    case argus::voice::v1::VOICE_LANGUAGE_ES:
      return VoiceLang::Es;
    case argus::voice::v1::VOICE_LANGUAGE_EN:
      return VoiceLang::En;
    default:
      break;
  }
  return VoiceLang::System;
}

argus::voice::v1::ReactionKind reactionKindToProto(ReactionKind kind)
{
  switch (kind) {
    case ReactionKind::Idle:
      return argus::voice::v1::REACTION_IDLE;
    case ReactionKind::Warm:
      return argus::voice::v1::REACTION_WARM;
    case ReactionKind::Thinking:
      return argus::voice::v1::REACTION_THINKING;
    case ReactionKind::Uncertain:
      return argus::voice::v1::REACTION_UNCERTAIN;
    case ReactionKind::Attentive:
      return argus::voice::v1::REACTION_ATTENTIVE;
    case ReactionKind::Recognizing:
      return argus::voice::v1::REACTION_RECOGNIZING;
    case ReactionKind::Curious:
      return argus::voice::v1::REACTION_CURIOUS;
    case ReactionKind::Acknowledging:
      return argus::voice::v1::REACTION_ACKNOWLEDGING;
    case ReactionKind::Confused:
      return argus::voice::v1::REACTION_CONFUSED;
    case ReactionKind::Alarmed:
      return argus::voice::v1::REACTION_ALARMED;
  }
  return argus::voice::v1::REACTION_IDLE;
}

// Keeps `speaking` false even when TTS synthesis throws.
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

// Next TTS chunk boundary in bytes (0 = keep buffering).
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
        return len;
      if (!std::isspace(static_cast<unsigned char>(text[j])))
        continue;
      if (firstSentence || i >= kMinSentenceChars)
        return j;
    }
    if ((c == ',' || c == ';' || c == ':') &&
        i >= kClauseMinChars &&
        i + 1 < len &&
        std::isspace(static_cast<unsigned char>(text[i + 1])) &&
        len - i - 1 >= kClauseTailChars) {
      return i + 1;
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
    : stt_(engines.stt), tts_(engines.tts), llm_(engines.llm),
      identity_(engines.identity)
{
  reactions_.init();
}

Reaction VoiceSessionService::emitReaction(Session& session,
                                          const ReactionSignals& signals)
{
  const Reaction reaction = reactions_.react(signals);
  argus::voice::v1::ServerFrame frame;
  auto* event = frame.mutable_event();
  event->set_reaction(reactionKindToProto(reaction.kind));
  event->set_intensity(reaction.intensity);
  event->set_because(reaction.because);
  sendFrame(session, std::move(frame));
  return reaction;
}

void VoiceSessionService::start(VoiceSessionSink& sink,
                                const argus::voice::v1::VoiceIdentity& identity)
{
  VoiceLang lang = voiceLangFromProto(identity.language());
  if (lang == VoiceLang::System)
    lang = voiceSystemLang();

  const std::string userName = identity.name();
  LOG_INFO << "Voice: session start user=" << identity.user_id()
           << " lang=" << voiceLangToString(lang)
           << " nameKnown=" << (userName.size() >= 2);

  auto session = std::make_shared<Session>();
  session->sink = &sink;
  session->lang = lang;
  session->userId = identity.user_id();
  session->role = voiceRoleToString(identity.role());
  session->nameKnown = userName.size() >= 2;
  session->denoise = ConfigService::getBool("vad.denoise");
  const double gateRms = ConfigService::getDouble("vad.denoise_gate_rms");
  session->denoiseGateRms = gateRms > 0.0 ? static_cast<float>(gateRms) : 0.0035F;
  session->history.push_back({"system", systemPrompt(session->lang)});

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
  sessions_[&sink] = std::move(session);
}

void VoiceSessionService::feedPcm(VoiceSessionSink& sink, const PcmFrame& pcm)
{
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(&sink);
    if (it == sessions_.end())
      return;
    session = it->second;
  }

  if (session->speaking)
    return;

  const size_t sampleCount = pcm.size / 2;
  std::vector<float> floats(sampleCount);
  for (size_t i = 0; i < sampleCount; ++i) {
    const int16_t s = static_cast<int16_t>(
        (static_cast<unsigned char>(pcm.data[i * 2]) |
         (static_cast<unsigned char>(pcm.data[i * 2 + 1]) << 8)));
    floats[i] = static_cast<float>(s) / 32768.0F;
  }

  {
    std::lock_guard<std::mutex> lock(session->pcmMutex);
    session->pcmQueue.insert(session->pcmQueue.end(), floats.begin(),
                             floats.end());
  }
  session->pcmCv.notify_one();
}

void VoiceSessionService::stop(VoiceSessionSink& sink)
{
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(&sink);
    if (it == sessions_.end())
      return;
    session = it->second;
    sessions_.erase(it);
  }
  session->active.store(false);
  session->pcmCv.notify_all();
  if (session->worker.joinable())
    session->worker.join();

  if (session->sink && session->sink->connected()) {
    argus::voice::v1::ServerFrame done;
    done.mutable_done()->set_session_id(0);
    sendFrame(*session, std::move(done));
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

    if (session->speaking)
      continue;

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
  session.interrupt.store(false);

  std::string userText;
  try {
    userText = stt_.transcribe(samples, 16000);
  }
  catch (const std::exception& e) {
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

  argus::voice::v1::ServerFrame sttFrame;
  sttFrame.mutable_stt()->set_text(userText);
  sttFrame.mutable_stt()->set_final(true);
  sendFrame(session, std::move(sttFrame));

  session.history.push_back({"user", userText});

  if (session.userId > 0 && !session.nameKnown) {
    if (const auto name = extractName(userText)) {
      identity_.updateUserName({.userId = session.userId,
                                .role = session.role,
                                .name = *name});
      session.nameKnown = true;
    }
  }

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
      if (!session.sink || !session.sink->connected())
        return;
      if (session.interrupt.load())
        return;

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

  session.vad.reset();
}

void VoiceSessionService::speak(Session& session, const std::string& text)
{
  if (text.empty() || !session.sink || !session.sink->connected())
    return;
  LOG_INFO << "Voice: speaking -> " << text.substr(0, 80);

  TtsRequest treq;
  treq.text = text;
  treq.lang = session.lang == VoiceLang::En ? TtsLang::EN : TtsLang::ES;
  treq.quality = TtsQuality::Auto;
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
  argus::voice::v1::ServerFrame assistantFrame;
  assistantFrame.mutable_assistant()->set_text(text);
  try {
    tts_.synthesizeStream(treq, [&](const std::vector<float>& chunk) {
      if (!session.sink || !session.sink->connected())
        return;
      if (session.interrupt.load())
        return;
      const auto raw = floatToInt16(chunk);
      std::vector<int16_t> resampled;
      resampler.process(raw.data(), raw.size(), resampled);
      if (!resampled.empty()) {
        argus::voice::v1::ServerFrame chunkFrame;
        chunkFrame.mutable_tts_chunk()->set_pcm(
            reinterpret_cast<const char*>(resampled.data()),
            static_cast<size_t>(resampled.size()) * sizeof(int16_t));
        sendFrame(session, std::move(chunkFrame));
      }
    });
  }
  catch (const std::exception& e) {
    LOG_WARN << "Voice: TTS failed: " << e.what();
  }
  catch (...) {
    LOG_WARN << "Voice: TTS failed (unknown error)";
  }
  sendFrame(session, std::move(assistantFrame));
}

void VoiceSessionService::skip(VoiceSessionSink& sink)
{
  std::shared_ptr<Session> session;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(&sink);
    if (it == sessions_.end())
      return;
    session = it->second;
  }
  LOG_INFO << "Voice: skip";
  session->interrupt.store(true);
}

void VoiceSessionService::sendFrame(Session& session,
                                    argus::voice::v1::ServerFrame frame) const
{
  if (!session.sink || !session.sink->connected())
    return;
  session.sink->sendServerFrame(std::move(frame));
}
