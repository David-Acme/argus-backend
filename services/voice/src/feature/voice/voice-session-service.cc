#include "voice-session-service.hxx"

#include <algorithm>
#include <utility>
#include <cctype>
#include <cmath>
#include <drogon/drogon.h>
#include <auth/user-role.hxx>
#include <config/config-service.hxx>

namespace
{

constexpr int kTargetRate = 16000;
constexpr size_t kMaxQueuedSamples = static_cast<size_t>(kTargetRate) * 30;

float rmsOf(const std::vector<float>& samples)
{
  if (samples.empty())
    return 0.0F;
  double sum = 0.0;
  for (const float s : samples)
    sum += static_cast<double>(s) * s;
  return static_cast<float>(std::sqrt(sum / static_cast<double>(samples.size())));
}

constexpr size_t kMinSentenceChars = 10;
constexpr size_t kClauseMinChars = 46;
constexpr size_t kClauseTailChars = 22;
constexpr size_t kHardMaxChars = 110;
constexpr size_t kHardMinCut = 28;

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

std::string unansweredLine(VoiceLang lang)
{
  return lang == VoiceLang::En ? "Sorry, I couldn't answer that right now."
                               : "Perdona, ahora mismo no he podido responder.";
}

constexpr size_t kHistoryLimit = 21;
constexpr size_t kHistoryKeep = 10;

void trimHistory(std::vector<ChatMessage>& history)
{
  if (history.size() <= kHistoryLimit)
    return;
  history.erase(history.begin() + 1, history.end() - kHistoryKeep);
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

constexpr size_t kMaxNotes = 6;
constexpr size_t kMaxNoteChars = 300;
constexpr size_t kMaxCameraChars = 64;
constexpr size_t kMaxSummaryChars = 200;
constexpr auto kNoticeFreshFor = std::chrono::seconds(20);
constexpr auto kNoticeSpacing = std::chrono::seconds(30);

std::string sanitizedLine(const std::string& text, size_t limit)
{
  std::string line;
  line.reserve(std::min(text.size(), limit));
  for (const char c : text) {
    if (line.size() >= limit)
      break;
    const auto byte = static_cast<unsigned char>(c);
    line.push_back(byte < 0x20 ? ' ' : c);
  }
  const auto first = line.find_first_not_of(' ');
  if (first == std::string::npos)
    return {};
  const auto last = line.find_last_not_of(' ');
  return line.substr(first, last - first + 1);
}

std::string cameraOffer(VoiceLang lang, const std::string& camera, const std::string& summary)
{
  if (lang == VoiceLang::En)
    return summary.empty()
               ? "Hey, something came up on the " + camera + " camera. Want me to show you?"
               : "Hey, something came up on the " + camera + " camera: " + summary + ". Want me to show you?";
  return summary.empty()
             ? "Oye, tengo algo en la cámara " + camera + ". ¿Quieres que te lo muestre?"
             : "Oye, tengo algo en la cámara " + camera + ": " + summary + ". ¿Quieres que te lo muestre?";
}

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
    if (lower.starts_with(p)) {
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

}

VoiceLang voiceSystemLang()
{
  return voiceLangFromString(ConfigService::getString("stt.language"));
}

VoiceListeningConfig resolveVoiceListeningConfig()
{
  VoiceListeningConfig config;
  if (ConfigService::hasKey("vad.denoise"))
    config.denoise = ConfigService::getBool("vad.denoise");
  if (const double gateRms = ConfigService::getDouble("vad.denoise_gate_rms"); gateRms > 0.0)
    config.denoiseGateRms = static_cast<float>(gateRms);
  if (const int guardMs = ConfigService::getInt("vad.barge_guard_ms"); guardMs > 0)
    config.bargeGuard = std::chrono::milliseconds(guardMs);
  return config;
}

VoiceSessionService::VoiceSessionService(const VoiceEngineSeam& engines)
    : stt_(engines.stt), tts_(engines.tts), llm_(engines.llm),
      identity_(engines.identity), vad_(engines.vad)
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
  argus::voice::v1::VoiceStart halfDuplex;
  *halfDuplex.mutable_identity() = identity;
  start(sink, halfDuplex);
}

void VoiceSessionService::start(VoiceSessionSink& sink,
                                const argus::voice::v1::VoiceStart& request)
{
  const argus::voice::v1::VoiceIdentity& identity = request.identity();
  const bool duplex = request.mode() == argus::voice::v1::VOICE_MODE_DUPLEX;
  {
    std::scoped_lock lock(mutex_);
    if (sessions_.contains(&sink)) {
      LOG_WARN << "Voice: a second start on a live session is ignored";
      return;
    }
  }

  VoiceLang lang = voiceLangFromProto(identity.language());
  if (lang == VoiceLang::System)
    lang = voiceSystemLang();

  const std::string userName = identity.name();
  LOG_INFO << "Voice: session start user=" << identity.user_id()
           << " lang=" << voiceLangToString(lang)
           << " nameKnown=" << (userName.size() >= 2)
           << " duplex=" << duplex;

  auto session = std::make_shared<Session>(vad_.createModel());
  session->sink = &sink;
  session->lang = lang;
  session->userId = identity.user_id();
  session->role = voiceRoleToString(identity.role());
  session->nameKnown = userName.size() >= 2;
  const VoiceListeningConfig listening = resolveVoiceListeningConfig();
  session->denoise = listening.denoise;
  session->denoiseGateRms = listening.denoiseGateRms;
  session->duplex = duplex;
  session->bargeGuard = listening.bargeGuard;
  session->history.push_back({"system", systemPrompt(session->lang)});

  const std::string greeting = greetingFor(session->lang, userName);
  session->history.push_back({"assistant", greeting});

  session->worker = std::thread([this, session, greeting] {
    if (session->duplex) {
      launchTurn(session, [this, greeting](Session& turn) { speak(turn, greeting); });
      duplexLoop(session);
      return;
    }
    speak(*session, greeting);
    workerLoop(session);
  });

  std::scoped_lock lock(mutex_);
  sessions_[&sink] = std::move(session);
}

void VoiceSessionService::feedPcm(VoiceSessionSink& sink, const PcmFrame& pcm)
{
  std::shared_ptr<Session> session;
  {
    std::scoped_lock lock(mutex_);
    auto it = sessions_.find(&sink);
    if (it == sessions_.end())
      return;
    session = it->second;
  }

  if (session->speaking && !session->duplex)
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
    std::scoped_lock lock(session->pcmMutex);
    auto& queue = session->pcmQueue;
    queue.insert(queue.end(), floats.begin(), floats.end());
    if (queue.size() > kMaxQueuedSamples)
      queue.erase(queue.begin(),
                  queue.begin() + static_cast<std::ptrdiff_t>(queue.size() - kMaxQueuedSamples));
  }
  session->pcmCv.notify_one();
}

void VoiceSessionService::stop(VoiceSessionSink& sink)
{
  std::shared_ptr<Session> session;
  {
    std::scoped_lock lock(mutex_);
    auto it = sessions_.find(&sink);
    if (it == sessions_.end())
      return;
    session = it->second;
    sessions_.erase(it);
  }
  std::stop_source cancellation;
  {
    std::scoped_lock lock(session->turnMutex);
    session->active.store(false);
    cancellation = session->turnStop;
  }
  cancellation.request_stop();
  session->pcmCv.notify_all();
  if (session->worker.joinable())
    session->worker.join();
  if (session->turnThread.joinable())
    session->turnThread.join();

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

    if (!session->speaking) {
      if (const auto notice = takeCameraNotice(*session)) {
        applyNotes(*session);
        deliverCameraNotice(*session, *notice);
      }
    }

    if (session->speaking)
      continue;

    std::vector<float> clean = cleanBatch(*session, batch);

    size_t offset = 0;
    while (offset < clean.size() && session->active.load()) {
      const size_t chunk = std::min<size_t>(512, clean.size() - offset);
      if (const auto turn = session->vad.process(
              {.samples = clean.data() + offset,
               .count = static_cast<int>(chunk)})) {
        if (!turn->samples.empty()) {
          try {
            processTurn(*session, turn->samples);
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

std::vector<float> VoiceSessionService::cleanBatch(Session& session,
                                                   std::vector<float>& batch)
{
  std::vector<float> clean;
  if (session.denoise) {
    const bool silent = rmsOf(batch) < session.denoiseGateRms &&
                        !session.denoiser.recentVoice();
    if (silent) {
      clean.swap(batch);
    } else {
      session.denoiser.process(batch, clean);
    }
    if ((++session.denoiseLogCounter % 25) == 0)
      LOG_INFO << "Voice: denoise prob=" << session.denoiser.lastVoiceProb();
  } else {
    clean.swap(batch);
  }
  return clean;
}

void VoiceSessionService::duplexLoop(const std::shared_ptr<Session>& session)
{
  bool wasListening = false;
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

    if (!listenState(*session).listening) {
      if (auto notice = takeCameraNotice(*session))
        launchTurn(session, [this, notice = std::move(*notice)](Session& active) {
          applyNotes(active);
          deliverCameraNotice(active, notice);
        });
    }

    const std::vector<float> clean = cleanBatch(*session, batch);

    size_t offset = 0;
    while (offset < clean.size() && session->active.load()) {
      const size_t chunk = std::min<size_t>(512, clean.size() - offset);
      const ListenState state = listenState(*session);
      if (state.listening) {
        wasListening = true;
        if (session->vad.listen({.samples = clean.data() + offset,
                                 .count = static_cast<int>(chunk),
                                 .armed = state.armed})) {
          wasListening = false;
          bargeIn(*session);
        }
      }
      else {
        if (wasListening) {
          session->vad.reset();
          wasListening = false;
        }
        auto turn = session->vad.process({.samples = clean.data() + offset,
                                          .count = static_cast<int>(chunk)});
        if (turn && !turn->samples.empty()) {
          launchTurn(session,
                     [this, samples = std::move(turn->samples)](Session& active) {
                       processTurn(active, samples);
                     });
        }
      }
      offset += chunk;
    }
  }
}

void VoiceSessionService::launchTurn(const std::shared_ptr<Session>& session,
                                     std::function<void(Session&)> body)
{
  if (session->turnThread.joinable())
    session->turnThread.join();
  {
    std::scoped_lock lock(session->duplexMutex);
    session->turn.running = true;
    session->turn.announced = false;
    session->turn.barged = false;
  }
  session->turnThread = std::thread([session, body = std::move(body)] {
    try {
      body(*session);
    }
    catch (const std::exception& e) {
      LOG_WARN << "Voice: turn failed: " << e.what();
      session->speaking.store(false);
    }
    catch (...) {
      LOG_WARN << "Voice: turn failed (unknown error)";
      session->speaking.store(false);
    }
    std::scoped_lock lock(session->duplexMutex);
    session->turn.running = false;
  });
}

VoiceSessionService::ListenState
VoiceSessionService::listenState(Session& session)
{
  const auto now = std::chrono::steady_clock::now();
  std::scoped_lock lock(session.duplexMutex);
  const DuplexTurn& turn = session.turn;
  return {.listening = !turn.barged && (turn.running || now < turn.playbackEnd),
          .armed = turn.announced && now >= turn.firstAudioAt + session.bargeGuard};
}

void VoiceSessionService::bargeIn(Session& session)
{
  std::stop_source cancellation;
  {
    std::scoped_lock lock(session.turnMutex);
    session.interrupt.store(true);
    cancellation = session.turnStop;
  }
  cancellation.request_stop();

  std::scoped_lock lock(session.duplexMutex);
  session.turn.barged = true;
  session.turn.playbackEnd = std::chrono::steady_clock::now();
  LOG_INFO << "Voice: barge-in interrupts turn " << session.turn.id;
  argus::voice::v1::ServerFrame frame;
  frame.mutable_interrupted()->set_id(session.turn.id);
  sendFrame(session, std::move(frame));
}

void VoiceSessionService::sendDuplexChunk(Session& session,
                                          argus::voice::v1::ServerFrame frame)
{
  std::scoped_lock lock(session.duplexMutex);
  if (session.turn.barged)
    return;
  if (!session.turn.announced) {
    session.turn.announced = true;
    session.turn.firstAudioAt = std::chrono::steady_clock::now();
    ++session.turn.id;
    argus::voice::v1::ServerFrame turnFrame;
    turnFrame.mutable_turn()->set_id(session.turn.id);
    sendFrame(session, std::move(turnFrame));
  }
  sendFrame(session, std::move(frame));
}

void VoiceSessionService::sendDuplexAssistant(Session& session,
                                              AssistantSend send)
{
  const auto now = std::chrono::steady_clock::now();
  std::scoped_lock lock(session.duplexMutex);
  if (session.turn.barged)
    return;
  if (session.turn.announced)
    send.frame.mutable_assistant()->set_turn_id(session.turn.id);
  const auto audible = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(static_cast<double>(send.samples) / kTargetRate));
  session.turn.playbackEnd = std::max(session.turn.playbackEnd, now) + audible;
  sendFrame(session, std::move(send.frame));
}

void VoiceSessionService::processTurn(Session& session,
                                      const std::vector<float>& samples)
{
  std::stop_token cancellation;
  {
    std::scoped_lock lock(session.turnMutex);
    if (!session.active.load())
      return;
    session.interrupt.store(false);
    session.turnStop = std::stop_source{};
    cancellation = session.turnStop.get_token();
  }

  applyNotes(session);

  std::string userText;
  try {
    userText = stt_.transcribe({.samples = samples,
                                .sampleRate = kTargetRate,
                                .language = voiceLangToString(session.lang)});
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
  LOG_INFO << "Voice: STT turn of " << userText.size() << " bytes";
  LOG_DEBUG << "Voice: STT -> " << userText;

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

  session.history.back().content += ReactionEngine::toneNote(
      reaction, session.lang == VoiceLang::En ? "en" : "es");

  ChatRequest req;
  req.clientActions = true;
  req.messages = session.history;
  req.userId = session.userId;
  req.role = userRoleFromString(session.role);
  req.lang = voiceLangToString(session.lang);

  std::string full;
  std::string pending;
  bool prefixStripped = false;
  bool firstSentenceSent = false;

  const TokenCallback onToken = [&](const std::string& token, bool) {
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
  };
  try {
    llm_.chatStream({.request = req,
                     .onToken = onToken,
                     .stats = nullptr,
                     .cancellation = cancellation,
                     .onAction = [this, &session](const ClientAction& action) {
                       argus::voice::v1::ServerFrame frame;
                       frame.mutable_action()->set_id(++session.actionSeq);
                       frame.mutable_action()->set_name(action.name);
                       frame.mutable_action()->set_arguments(action.arguments);
                       LOG_INFO << "Voice: client action " << action.name;
                       sendFrame(session, std::move(frame));
                     }});
  }
  catch (const std::exception& e) {
    if (cancellation.stop_requested())
      LOG_INFO << "Voice: LLM generation cancelled by an interrupt";
    else
      LOG_WARN << "Voice: LLM failed: " << e.what();
  }
  catch (...) {
    LOG_WARN << "Voice: LLM failed (unknown error)";
  }

  if (!pending.empty() && !session.interrupt.load())
    speak(session, pending);

  if (full.empty()) {
    session.history.pop_back();
    if (!session.interrupt.load())
      speak(session, unansweredLine(session.lang));
  }
  else {
    session.history.push_back({.role = "assistant", .content = full});
    trimHistory(session.history);
  }
  session.speaking = false;

  if (!session.duplex)
    session.vad.reset();
}

void VoiceSessionService::speak(Session& session, const std::string& text)
{
  if (text.empty() || !session.sink || !session.sink->connected())
    return;
  std::stop_token cancellation;
  {
    std::scoped_lock lock(session.turnMutex);
    if (!session.active.load() || session.interrupt.load())
      return;
    cancellation = session.turnStop.get_token();
  }
  LOG_INFO << "Voice: speaking " << text.size() << " bytes";
  LOG_DEBUG << "Voice: speaking -> " << text.substr(0, 80);

  TtsRequest treq;
  treq.text = text;
  treq.lang = session.lang == VoiceLang::En ? TtsLang::EN : TtsLang::ES;
  treq.quality = TtsQuality::Auto;
  int ttsRate = kTargetRate;
  try {
    treq.speed = tts_.defaultSpeed(cancellation);
    ttsRate = tts_.sampleRate(cancellation);
  }
  catch (const std::exception& e) {
    LOG_WARN << "Voice: TTS unavailable: " << e.what();
  }
  AudioResampler resampler(
      {.sourceRate = ttsRate > 0 ? ttsRate : kTargetRate,
       .targetRate = kTargetRate});

  SpeakingGuard speakingGuard(session.speaking);
  size_t spokenSamples = 0;
  argus::voice::v1::ServerFrame assistantFrame;
  assistantFrame.mutable_assistant()->set_text(text);
  try {
    tts_.synthesizeStream({.request = treq,
                           .onChunk = [&](const std::vector<float>& chunk) {
      if (!session.sink || !session.sink->connected())
        return;
      if (session.interrupt.load())
        return;
      const auto raw = floatToInt16(chunk);
      const std::vector<int16_t> resampled =
          resampler.process(raw.data(), raw.size());
      if (!resampled.empty()) {
        argus::voice::v1::ServerFrame chunkFrame;
        chunkFrame.mutable_tts_chunk()->set_pcm(
            reinterpret_cast<const char*>(resampled.data()),
            static_cast<size_t>(resampled.size()) * sizeof(int16_t));
        if (session.duplex) {
          spokenSamples += resampled.size();
          sendDuplexChunk(session, std::move(chunkFrame));
        }
        else {
          sendFrame(session, std::move(chunkFrame));
        }
      }
    },
                           .cancellation = cancellation});
  }
  catch (const std::exception& e) {
    LOG_WARN << "Voice: TTS failed: " << e.what();
  }
  catch (...) {
    LOG_WARN << "Voice: TTS failed (unknown error)";
  }
  if (session.duplex) {
    sendDuplexAssistant(session, {.frame = std::move(assistantFrame),
                                  .samples = spokenSamples});
    return;
  }
  sendFrame(session, std::move(assistantFrame));
}

void VoiceSessionService::skip(VoiceSessionSink& sink)
{
  std::shared_ptr<Session> session;
  {
    std::scoped_lock lock(mutex_);
    auto it = sessions_.find(&sink);
    if (it == sessions_.end())
      return;
    session = it->second;
  }
  LOG_INFO << "Voice: skip";
  std::stop_source cancellation;
  {
    std::scoped_lock lock(session->turnMutex);
    session->interrupt.store(true);
    cancellation = session->turnStop;
  }
  cancellation.request_stop();
  if (session->duplex) {
    std::scoped_lock lock(session->duplexMutex);
    session->turn.playbackEnd = std::chrono::steady_clock::now();
  }
}

void VoiceSessionService::context(VoiceSessionSink& sink,
                                  const argus::voice::v1::VoiceContext& context)
{
  std::shared_ptr<Session> session;
  {
    std::scoped_lock lock(mutex_);
    auto it = sessions_.find(&sink);
    if (it == sessions_.end())
      return;
    session = it->second;
  }
  std::scoped_lock lock(session->noticeMutex);
  if (context.kind() == argus::voice::v1::VOICE_CONTEXT_CAMERA_EVENT) {
    const std::string camera = sanitizedLine(context.camera(), kMaxCameraChars);
    if (camera.empty())
      return;
    session->pendingCamera = CameraNotice{.camera = camera,
                                          .summary = sanitizedLine(context.text(), kMaxSummaryChars),
                                          .at = std::chrono::steady_clock::now()};
    return;
  }
  const std::string note = sanitizedLine(context.text(), kMaxNoteChars);
  if (note.empty())
    return;
  session->pendingNotes.push_back(note);
  if (session->pendingNotes.size() > kMaxNotes)
    session->pendingNotes.erase(session->pendingNotes.begin());
}

void VoiceSessionService::applyNotes(Session& session)
{
  {
    std::scoped_lock lock(session.noticeMutex);
    if (session.pendingNotes.empty())
      return;
    for (auto& note : session.pendingNotes)
      session.notes.push_back(std::move(note));
    session.pendingNotes.clear();
  }
  if (session.notes.size() > kMaxNotes)
    session.notes.erase(session.notes.begin(),
                        session.notes.end() - static_cast<std::ptrdiff_t>(kMaxNotes));
  if (session.history.empty() || session.history.front().role != "system")
    return;
  std::string prompt = systemPrompt(session.lang);
  for (const auto& note : session.notes)
    prompt += "\n" + note;
  session.history.front().content = std::move(prompt);
}

std::optional<VoiceSessionService::CameraNotice>
VoiceSessionService::takeCameraNotice(Session& session)
{
  std::scoped_lock lock(session.noticeMutex);
  auto notice = std::exchange(session.pendingCamera, std::nullopt);
  if (!notice)
    return std::nullopt;
  const auto now = std::chrono::steady_clock::now();
  if (now - notice->at > kNoticeFreshFor || now - session.lastNoticeAt < kNoticeSpacing)
    return std::nullopt;
  session.lastNoticeAt = now;
  return notice;
}

void VoiceSessionService::deliverCameraNotice(Session& session, const CameraNotice& notice)
{
  {
    std::scoped_lock lock(session.turnMutex);
    if (!session.active.load())
      return;
    session.interrupt.store(false);
    session.turnStop = std::stop_source{};
  }
  const std::string offer = cameraOffer(session.lang, notice.camera, notice.summary);
  LOG_INFO << "Voice: offering camera " << notice.camera;
  speak(session, offer);
  session.history.push_back({.role = "assistant", .content = offer});
  trimHistory(session.history);
}

void VoiceSessionService::sendFrame(Session& session,
                                    argus::voice::v1::ServerFrame frame) const
{
  if (!session.sink || !session.sink->connected())
    return;
  session.sink->sendServerFrame(std::move(frame));
}
