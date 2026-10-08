#include "voice-session-service.hxx"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <random>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <drogon/drogon.h>
#include <auth/role-access.hxx>
#include <auth/user-role.hxx>
#include <config/config-service.hxx>
#include <feature/voice/offer-reply.hxx>
#include <json/json.h>

namespace
{

constexpr int kTargetRate = 16000;
constexpr auto kIdleTick = std::chrono::milliseconds(150);

std::string mintCallKey()
{
  constexpr std::string_view kHex = "0123456789abcdef";
  constexpr size_t kKeyChars = 32;
  std::random_device entropy;
  std::string key;
  key.reserve(kKeyChars);
  while (key.size() < kKeyChars) {
    const unsigned word = entropy();
    for (unsigned shift = 0; shift < 32U && key.size() < kKeyChars; shift += 4U)
      key.push_back(kHex[(word >> shift) & 0xFU]);
  }
  return key;
}

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
constexpr size_t kLeadInMinChars = 2;
constexpr size_t kLeadInMaxChars = 24;
constexpr size_t kHardMinCut = 28;
constexpr std::ptrdiff_t kMaxNameWords = 3;

struct Greeting
{
  std::string withName;
  std::string askName;
};

const std::unordered_map<VoiceLang, Greeting>& greetings()
{
  static const std::unordered_map<VoiceLang, Greeting> map = {
      {VoiceLang::Es,
       {.withName = "Hola {name}, soy Argus. ¿Qué tal tu día?",
        .askName = "Hola, soy Argus, tu asistente local. ¿Cómo te llamas?"}},
      {VoiceLang::En,
       {.withName = "Hi {name}, it's Argus. How's your day going?",
        .askName = "Hi, I'm Argus, your local assistant. What's your name?"}},
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

std::string langCode(VoiceLang lang)
{
  return lang == VoiceLang::En ? "en" : "es";
}

int64_t elapsedMs(std::chrono::steady_clock::time_point from,
                  std::chrono::steady_clock::time_point to)
{
  if (from == std::chrono::steady_clock::time_point{} || to == std::chrono::steady_clock::time_point{})
    return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
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
constexpr size_t kMaxAnnouncementChars = 400;
constexpr size_t kMaxPendingAnnouncements = 4;
constexpr auto kAnnouncementFreshFor = std::chrono::seconds(60);
constexpr size_t kMaxSituationChars = 900;
constexpr size_t kMaxCameraChars = 64;
constexpr size_t kMaxSummaryChars = 200;
constexpr size_t kMaxDetailChars = 120;
constexpr size_t kMaxRememberedActions = 32;
constexpr size_t kMaxPendingFailures = 4;
constexpr auto kNoticeFreshFor = std::chrono::seconds(20);
constexpr auto kFailureFreshFor = std::chrono::seconds(30);
constexpr auto kNoticeSpacing = std::chrono::seconds(30);
constexpr auto kOfferAnswerWindow = std::chrono::seconds(45);
constexpr size_t kSpeakerMinSamples = static_cast<size_t>(kTargetRate) * 2;
constexpr size_t kSpeakerMaxSamples = static_cast<size_t>(kTargetRate) * 6;
constexpr auto kSpeakerGrace = std::chrono::milliseconds(300);

std::string offerSummary(const std::string& summary)
{
  std::string text = summary;
  while (!text.empty() && (text.back() == '.' || text.back() == ' '))
    text.pop_back();
  if (!text.empty() && text.front() >= 'A' && text.front() <= 'Z' &&
      (text.size() < 2 || text[1] < 'A' || text[1] > 'Z'))
    text.front() = static_cast<char>(text.front() - 'A' + 'a');
  return text;
}

std::string compactJson(const Json::Value& value)
{
  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  return Json::writeString(builder, value);
}

struct ActionLine
{
  std::string_view name;
  std::string_view es;
  std::string_view en;
};

constexpr std::array kActionLines{
    ActionLine{.name = "app.show_camera",
               .es = "No he podido mostrarte la cámara",
               .en = "I couldn't show you the camera"},
    ActionLine{.name = "app.open",
               .es = "No he podido abrir esa sección",
               .en = "I couldn't open that section"},
    ActionLine{.name = "app.set_guard_mode",
               .es = "No he podido cambiar el modo de vigilancia",
               .en = "I couldn't change the guard mode"},
};

struct ActionFailureText
{
  VoiceLang lang{VoiceLang::System};
  std::string_view name;
  std::string_view detail;
};

std::string actionFailureLine(const ActionFailureText& failure)
{
  const auto line = std::ranges::find(kActionLines, failure.name, &ActionLine::name);
  const bool en = failure.lang == VoiceLang::En;
  std::string text(line == kActionLines.end()
                       ? (en ? std::string_view("I couldn't do that in the app")
                             : std::string_view("No he podido hacerlo en la app"))
                       : (en ? line->en : line->es));
  if (!failure.detail.empty()) {
    text += ": ";
    text += failure.detail;
  }
  text += ".";
  return text;
}

std::string actionFailureEvent(const ActionFailureText& failure)
{
  std::string event = "The app could not complete ";
  event += failure.name;
  if (!failure.detail.empty()) {
    event += ": ";
    event += failure.detail;
  }
  event += ".";
  return event;
}

struct CameraOfferText
{
  VoiceLang lang{VoiceLang::System};
  std::string camera;
  std::string summary;
};

std::string cameraOffer(const CameraOfferText& offer)
{
  const std::string summary = offerSummary(offer.summary);
  const std::string& camera = offer.camera;
  if (offer.lang == VoiceLang::En)
    return summary.empty()
               ? "Hey, something came up on the " + camera + " camera. Want me to show you?"
               : "Hey, something came up on the " + camera + " camera: " + summary + ". Want me to show you?";
  return summary.empty()
             ? "Oye, tengo algo en la cámara " + camera + ". ¿Quieres que te lo muestre?"
             : "Oye, tengo algo en la cámara " + camera + ": " + summary + ". ¿Quieres que te lo muestre?";
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
    const bool leadIn = firstSentence && c == ',' && i >= kLeadInMinChars && i < kLeadInMaxChars;
    if (leadIn && i + 1 < len && std::isspace(static_cast<unsigned char>(text[i + 1])))
      return i + 1;
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

bool opensClause(const std::string& lower, size_t pos)
{
  const auto before = lower.find_last_not_of(' ', pos == 0 ? 0 : pos - 1);
  if (pos == 0 || before == std::string::npos)
    return true;
  const char last = lower[before];
  if (last == ',' || last == '.' || last == '!' || last == '?' || last == ';')
    return true;
  return lower.substr(0, before + 1) == "hola" || lower.substr(0, before + 1) == "pues";
}

std::optional<std::string> extractName(const std::string& text)
{
  std::string lower = text;
  for (auto& c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  for (const std::string_view pattern : {"mi nombre es ", "me llamo ",
                                         "yo soy ", "soy "}) {
    const auto pos = lower.find(pattern);
    if (pos == std::string::npos || !opensClause(lower, pos))
      continue;
    std::string name = text.substr(pos + pattern.size());
    while (!name.empty() &&
           std::isspace(static_cast<unsigned char>(name.front())))
      name.erase(name.begin());
    const auto end = std::ranges::find_if(name, [](unsigned char c) {
      return c == '.' || c == ',' || c == '!' || c == '?';
    });
    name = std::string(name.begin(), end);
    while (!name.empty() &&
           std::isspace(static_cast<unsigned char>(name.back())))
      name.pop_back();
    const auto words = std::ranges::count(name, ' ') + 1;
    if (name.size() >= 2 && name.size() <= 40 && words <= kMaxNameWords) {
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
      identity_(engines.identity), vad_(engines.vad), speaker_(engines.speaker),
      farewells_(engines.tts)
{
  reactions_.init();
}

void VoiceSessionService::warmFarewells()
{
  farewells_.startWarming();
}

FarewellAudio VoiceSessionService::farewellAudio(const FarewellKey& key) const
{
  return farewells_.audio(key);
}

VoiceLang VoiceSessionService::langOf(const argus::voice::v1::VoiceIdentity& identity)
{
  const VoiceLang lang = voiceLangFromProto(identity.language());
  return lang == VoiceLang::System ? voiceSystemLang() : lang;
}

bool VoiceSessionService::farewell(VoiceSessionSink& sink, FarewellReason reason)
{
  const auto session = sessionOf(sink);
  if (!session)
    return false;
  session->muted.store(true);
  {
    std::scoped_lock lock(session->pcmMutex);
    session->pcmRing.clear();
  }
  std::stop_source cancellation;
  {
    std::scoped_lock lock(session->turnMutex);
    session->interrupt.store(true);
    session->farewell.store(true);
    cancellation = session->turnStop;
  }
  cancellation.request_stop();
  session->callStop.request_stop();
  const FarewellAudio audio = farewells_.audio({.lang = session->lang, .reason = reason});
  LOG_INFO << "Voice: farewell (" << (audio.pcm ? "cached line" : "no line cached, cut") << ")";
  if (!audio.pcm || !sink.connected())
    return false;
  constexpr std::size_t kChunkSamples = FarewellCache::kSampleRate / 5;
  const std::vector<int16_t>& pcm = *audio.pcm;
  for (std::size_t offset = 0; offset < pcm.size(); offset += kChunkSamples) {
    const std::size_t count = std::min(kChunkSamples, pcm.size() - offset);
    argus::voice::v1::ServerFrame chunk;
    chunk.mutable_tts_chunk()->set_pcm(reinterpret_cast<const char*>(pcm.data() + offset),
                                       count * sizeof(int16_t));
    sink.sendServerFrame(std::move(chunk));
  }
  argus::voice::v1::ServerFrame assistant;
  assistant.mutable_assistant()->set_text(audio.text);
  sink.sendServerFrame(std::move(assistant));
  return true;
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
  const bool resume = request.resume();
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

  const std::string& userName = identity.name();
  LOG_INFO << "Voice: session start user=" << identity.user_id()
           << " lang=" << voiceLangToString(lang)
           << " nameKnown=" << (userName.size() >= 2)
           << " duplex=" << duplex << " resume=" << resume;

  auto session = std::make_shared<Session>(SessionInit{.model = vad_.createModel(), .lang = lang});
  session->sink = &sink;
  session->userId = identity.user_id();
  session->deviceHash = identity.device_hash();
  session->callKey = mintCallKey();
  session->role = identity.has_role() ? voiceRoleToString(identity.role())
                                      : userRoleToString(UserRole::Unknown);
  session->nameKnown = userName.size() >= 2;
  session->callId = "voice-" + std::to_string(identity.user_id()) + "-" +
                    std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count());
  const VoiceListeningConfig listening = resolveVoiceListeningConfig();
  session->denoise = listening.denoise;
  session->denoiseGateRms = listening.denoiseGateRms;
  session->duplex = duplex;
  session->bargeGuard = listening.bargeGuard;

  const std::string opening = sanitizedLine(request.opening_line(), kMaxAnnouncementChars);
  const std::string greeting = resume            ? std::string()
                               : !opening.empty() ? opening
                                                  : greetingFor(session->lang, userName);
  if (!greeting.empty())
    session->history.addAssistant(greeting);

  session->worker = std::thread([this, session, greeting] {
    if (greeting.empty()) {
      primeLlm(*session);
      if (session->duplex)
        duplexLoop(session);
      else
        workerLoop(session);
      return;
    }
    if (session->duplex) {
      launchTurn(session, [this, greeting](Session& turn) {
        speak(turn, greeting);
        primeLlm(turn);
      });
      duplexLoop(session);
      return;
    }
    speak(*session, greeting);
    primeLlm(*session);
    workerLoop(session);
  });

  std::scoped_lock lock(mutex_);
  sessions_[&sink] = std::move(session);
}

std::shared_ptr<VoiceSessionService::Session>
VoiceSessionService::sessionOf(VoiceSessionSink& sink) const
{
  std::scoped_lock lock(mutex_);
  const auto it = sessions_.find(&sink);
  return it == sessions_.end() ? nullptr : it->second;
}

void VoiceSessionService::feedPcm(VoiceSessionSink& sink, const PcmFrame& pcm)
{
  thread_local std::vector<float> floats;
  const size_t sampleCount = pcm.size / 2;
  floats.resize(sampleCount);
  for (size_t i = 0; i < sampleCount; ++i) {
    const auto s = static_cast<int16_t>(
        (static_cast<unsigned char>(pcm.data[i * 2]) |
         (static_cast<unsigned char>(pcm.data[i * 2 + 1]) << 8)));
    floats[i] = static_cast<float>(s) / 32768.0F;
  }
  feedSamples(sink, floats);
}

void VoiceSessionService::feedSamples(VoiceSessionSink& sink, std::span<const float> samples)
{
  const auto session = sessionOf(sink);
  if (!session)
    return;

  if (session->muted.load() || (session->speaking && !session->duplex))
    return;

  {
    std::scoped_lock lock(session->pcmMutex);
    session->pcmRing.push(samples.data(), samples.size());
  }
  session->pcmCv.notify_one();
}

bool VoiceSessionService::announce(int64_t userId, const std::string& text)
{
  const std::string line = sanitizedLine(text, kMaxAnnouncementChars);
  if (userId <= 0 || line.empty())
    return false;
  std::vector<std::shared_ptr<Session>> targets;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& [sink, session] : sessions_)
      if (session->userId == userId && session->active.load())
        targets.push_back(session);
  }
  for (const auto& session : targets) {
    {
      std::scoped_lock lock(session->noticeMutex);
      session->pendingAnnouncements.push_back(
          {.text = line, .at = std::chrono::steady_clock::now()});
      if (session->pendingAnnouncements.size() > kMaxPendingAnnouncements)
        session->pendingAnnouncements.pop_front();
    }
    session->pcmCv.notify_one();
  }
  LOG_INFO << "Voice: announcement for user " << userId << " delivered to "
           << targets.size() << " call(s)";
  return !targets.empty();
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
  session->callStop.request_stop();
  session->pcmCv.notify_all();
  if (session->worker.joinable())
    session->worker.join();
  if (session->turnThread.joinable())
    session->turnThread.join();
  if (session->primeThread.joinable())
    session->primeThread.join();
  if (session->speakerThread.joinable())
    session->speakerThread.join();
  if (session->speakerHeard)
    speaker_.closeCall(session->callKey);

  if (session->sink && session->sink->connected()) {
    argus::voice::v1::ServerFrame done;
    done.mutable_done()->set_session_id(0);
    sendFrame(*session, std::move(done));
  }
}

bool VoiceSessionService::nextBatch(Session& session)
{
  std::unique_lock<std::mutex> lock(session.pcmMutex);
  session.pcmCv.wait_for(lock, kIdleTick, [&] {
    return session.pcmRing.size() > 0 || !session.active.load();
  });
  if (!session.active.load())
    return false;
  session.pcmRing.drainInto(session.batch);
  return true;
}

void VoiceSessionService::resetListening(Session& session)
{
  session.vad.reset();
  session.denoiser.reset();
  session.denoiseBypassed = false;
}

void VoiceSessionService::workerLoop(const std::shared_ptr<Session>& session)
{
  while (session->active.load()) {
    if (!nextBatch(*session))
      break;
    if (session->vadResetPending.exchange(false)) {
      resetListening(*session);
      followUtterance(*session);
    }

    if (!session->speaking && !session->vad.inSpeech()) {
      if (const auto notice = takeNotice(*session))
        deliverNotice(*session, *notice);
    }

    if (session->speaking || session->batch.empty())
      continue;

    const std::span<const float> clean = cleanBatch(*session);

    size_t offset = 0;
    while (offset < clean.size() && session->active.load()) {
      const size_t chunk = std::min<size_t>(512, clean.size() - offset);
      if (const auto turn = session->vad.process(
              {.samples = clean.data() + offset,
               .count = static_cast<int>(chunk)})) {
        auto heard = takeTranscript(*session);
        if (!turn->samples.empty()) {
          try {
            processTurn(*session, {.samples = turn->samples, .transcript = std::move(heard)});
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
      else {
        followUtterance(*session);
      }
      offset += chunk;
    }
  }
}

std::span<const float> VoiceSessionService::cleanBatch(Session& session)
{
  if (!session.denoise)
    return session.batch;
  const bool silent = rmsOf(session.batch) < session.denoiseGateRms &&
                      !session.denoiser.recentVoice();
  if (silent) {
    session.denoiseBypassed = true;
    return session.batch;
  }
  if (session.denoiseBypassed) {
    session.denoiser.resync();
    session.denoiseBypassed = false;
  }
  session.denoiser.process(session.batch, session.cleaned);
  if ((++session.denoiseLogCounter % 25) == 0)
    LOG_DEBUG << "Voice: denoise prob=" << session.denoiser.lastVoiceProb();
  return session.cleaned;
}

void VoiceSessionService::duplexLoop(const std::shared_ptr<Session>& session)
{
  bool wasListening = false;
  while (session->active.load()) {
    if (!nextBatch(*session))
      break;
    if (session->vadResetPending.exchange(false)) {
      resetListening(*session);
      followUtterance(*session);
      wasListening = false;
    }

    if (!listenState(*session).listening && !session->vad.inSpeech()) {
      if (auto notice = takeNotice(*session))
        launchTurn(session, [this, notice = std::move(*notice)](Session& active) {
          deliverNotice(active, notice);
        });
    }

    if (session->batch.empty())
      continue;

    const std::span<const float> clean = cleanBatch(*session);

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
        followUtterance(*session);
      }
      else {
        if (wasListening) {
          session->vad.endListening();
          wasListening = false;
        }
        auto turn = session->vad.process({.samples = clean.data() + offset,
                                          .count = static_cast<int>(chunk)});
        if (!turn)
          followUtterance(*session);
        else if (auto heard = takeTranscript(*session); !turn->samples.empty()) {
          launchTurn(session,
                     [this, samples = std::move(turn->samples), heard = std::move(heard)](Session& active) {
                       processTurn(active, {.samples = samples, .transcript = heard});
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

bool VoiceSessionService::sendDuplexChunk(Session& session,
                                          argus::voice::v1::ServerFrame frame)
{
  const auto now = std::chrono::steady_clock::now();
  const size_t samples = frame.tts_chunk().pcm().size() / sizeof(int16_t);
  const auto audible = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(static_cast<double>(samples) / kTargetRate));
  std::scoped_lock lock(session.duplexMutex);
  if (session.turn.barged)
    return false;
  session.turn.playbackEnd = std::max(session.turn.playbackEnd, now) + audible;
  if (!session.turn.announced) {
    session.turn.announced = true;
    session.turn.firstAudioAt = now;
    ++session.turn.id;
    argus::voice::v1::ServerFrame turnFrame;
    turnFrame.mutable_turn()->set_id(session.turn.id);
    sendFrame(session, std::move(turnFrame));
  }
  sendFrame(session, std::move(frame));
  return true;
}

bool VoiceSessionService::sendDuplexAssistant(Session& session,
                                              argus::voice::v1::ServerFrame frame)
{
  std::scoped_lock lock(session.duplexMutex);
  if (session.turn.barged)
    return false;
  if (session.turn.announced)
    frame.mutable_assistant()->set_turn_id(session.turn.id);
  sendFrame(session, std::move(frame));
  return true;
}

void VoiceSessionService::followUtterance(Session& session)
{
  if (!session.listening)
    std::ignore = takeTranscript(session);
  session.listening->follow(session.vad);
}

std::shared_ptr<TurnTranscript> VoiceSessionService::takeTranscript(Session& session)
{
  auto heard = std::move(session.listening);
  session.listening = std::make_shared<TurnTranscript>(TurnTranscriptInput{
      .stt = stt_,
      .stream = {.sampleRate = kTargetRate, .language = voiceLangToString(session.lang)},
      .flushSilenceFrames = TurnTranscript::flushFramesFor(session.vad.minSilenceFrames())});
  return heard;
}

VoiceSessionService::Transcript VoiceSessionService::transcribe(Session& session, const HeardTurn& heard)
{
  if (heard.transcript) {
    if (auto text = heard.transcript->finish(heard.samples))
      return {.text = std::move(*text), .streamed = true};
  }
  return {.text = stt_.transcribe({.samples = heard.samples,
                                   .sampleRate = kTargetRate,
                                   .language = voiceLangToString(session.lang)}),
          .streamed = false};
}

void VoiceSessionService::processTurn(Session& session,
                                      const std::vector<float>& samples)
{
  processTurn(session, {.samples = samples, .transcript = nullptr});
}

void VoiceSessionService::processTurn(Session& session, const HeardTurn& heard)
{
  const std::vector<float>& samples = heard.samples;
  LOG_DEBUG << "Voice: turn of " << samples.size() << " samples";
  TurnClock clock{.detected = std::chrono::steady_clock::now(),
                  .transcribed = {},
                  .firstToken = {},
                  .firstAudio = {}};
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
  const auto speakerProbe = probeSpeaker(session, samples);
  const std::string lang = langCode(session.lang);
  const ReactionSignals sttFailed{.text = {}, .lang = lang, .sttFailed = true};

  std::string userText;
  bool streamed = false;
  try {
    auto transcript = transcribe(session, heard);
    userText = std::move(transcript.text);
    streamed = transcript.streamed;
  }
  catch (const std::exception& e) {
    LOG_WARN << "Voice: STT failed: " << e.what();
    emitReaction(session, sttFailed);
    return;
  }
  clock.transcribed = std::chrono::steady_clock::now();
  if (userText.empty()) {
    LOG_WARN << "Voice: STT returned empty for a VAD turn";
    emitReaction(session, sttFailed);
    return;
  }
  LOG_INFO << "Voice: STT turn of " << userText.size() << " bytes";
  LOG_DEBUG << "Voice: STT -> " << userText;

  argus::voice::v1::ServerFrame sttFrame;
  sttFrame.mutable_stt()->set_text(userText);
  sttFrame.mutable_stt()->set_final(true);
  sendFrame(session, std::move(sttFrame));

  session.history.addUser(userText);
  noteSpeaker(session, awaitSpeaker(speakerProbe));

  if (session.userId > 0 && !session.nameKnown) {
    if (const auto name = extractName(userText)) {
      identity_.updateUserName({.userId = session.userId,
                                .role = session.role,
                                .name = *name});
      session.nameKnown = true;
    }
  }

  const Reaction reaction =
      emitReaction(session, {.text = userText, .lang = lang, .sttFailed = false});
  session.history.addTone(ReactionEngine::toneNote(reaction, lang));

  if (answerOffer(session, userText)) {
    LOG_INFO << "Voice: turn latency stt_ms=" << elapsedMs(clock.detected, clock.transcribed)
             << " answered_offer";
    return;
  }

  const ChatRequest req = turnRequest(session);

  std::string full;
  std::string pending;
  std::string spoken;
  bool prefixStripped = false;
  bool firstSentenceSent = false;

  const auto say = [&](const std::string& text) {
    const SpeakOutcome outcome = speak(session, text);
    if (!outcome.audible)
      return;
    if (clock.firstAudio == std::chrono::steady_clock::time_point{})
      clock.firstAudio = outcome.firstAudioAt;
    spoken += text;
  };

  const TokenCallback onToken = [&](const std::string& token, bool) {
    if (!session.sink || !session.sink->connected())
      return;
    if (session.interrupt.load())
      return;
    if (clock.firstToken == std::chrono::steady_clock::time_point{} && !token.empty())
      clock.firstToken = std::chrono::steady_clock::now();

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
      say(sentence);
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
                       rememberAction(session, action);
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
    say(pending);

  const bool interrupted = session.interrupt.load();
  if (full.empty()) {
    session.history.rollbackUser();
    if (!interrupted)
      speak(session, unansweredLine(session.lang));
  }
  else if (interrupted) {
    const std::string heard = sanitizedBlock(spoken, spoken.size());
    if (heard.empty())
      session.history.rollbackUser();
    else
      session.history.addAssistant(heard);
  }
  else {
    session.history.addAssistant(full);
  }
  const bool trimmed = session.history.trim();
  session.speaking = false;

  LOG_INFO << "Voice: turn latency stt_ms=" << elapsedMs(clock.detected, clock.transcribed)
           << (streamed ? " stt=stream" : " stt=unary")
           << " llm_first_token_ms=" << elapsedMs(clock.transcribed, clock.firstToken)
           << " tts_first_audio_ms=" << elapsedMs(clock.firstToken, clock.firstAudio)
           << " total_ms=" << elapsedMs(clock.detected, clock.firstAudio)
           << (interrupted ? " interrupted" : "");

  if (!session.duplex)
    session.vad.reset();
  if (trimmed)
    primeLlm(session);
}

std::shared_ptr<VoiceSessionService::SpeakerProbe>
VoiceSessionService::probeSpeaker(Session& session, const std::vector<float>& samples)
{
  if (samples.size() < kSpeakerMinSamples)
    return nullptr;
  if (session.speakerProbe) {
    std::scoped_lock lock(session.speakerProbe->mutex);
    if (!session.speakerProbe->finished)
      return nullptr;
  }
  if (session.speakerThread.joinable())
    session.speakerThread.join();
  auto probe = std::make_shared<SpeakerProbe>();
  session.speakerProbe = probe;
  std::vector<float> clip(samples.begin(),
                          samples.begin() + static_cast<std::ptrdiff_t>(std::min(samples.size(), kSpeakerMaxSamples)));
  session.speakerHeard = true;
  session.speakerThread = std::thread([this, probe, clip = std::move(clip), userId = session.userId,
                                       deviceHash = session.deviceHash, callKey = session.callKey] {
    std::optional<VoiceSpeaker> found;
    try {
      found = speaker_.identify({.samples = clip,
                                 .sampleRate = kTargetRate,
                                 .userId = userId,
                                 .deviceHash = deviceHash,
                                 .callKey = callKey});
    }
    catch (const std::exception& e) {
      LOG_DEBUG << "Voice: speaker identification skipped: " << e.what();
    }
    {
      std::scoped_lock lock(probe->mutex);
      probe->speaker = std::move(found);
      probe->finished = true;
    }
    probe->done.notify_all();
  });
  return probe;
}

std::optional<VoiceSpeaker> VoiceSessionService::awaitSpeaker(const std::shared_ptr<SpeakerProbe>& probe)
{
  if (!probe)
    return std::nullopt;
  std::unique_lock lock(probe->mutex);
  probe->done.wait_for(lock, kSpeakerGrace, [&probe] { return probe->finished; });
  return probe->speaker;
}

void VoiceSessionService::noteSpeaker(Session& session, const std::optional<VoiceSpeaker>& speaker)
{
  if (!speaker || speaker->userId == session.lastSpeakerId)
    return;
  const bool holder = speaker->userId == session.userId;
  const bool first = session.lastSpeakerId == 0;
  session.lastSpeakerId = speaker->userId;
  if (holder && first)
    return;
  LOG_INFO << "Voice: the voice of this turn is " << (holder ? "the account holder's" : "another enrolled user's");
  if (holder) {
    session.history.addEvent("The account holder is speaking again.");
    return;
  }
  const std::string who = speaker->name.empty() ? std::string("another member of the household") : speaker->name;
  session.history.addEvent("The last message was spoken by a voice that matches " + who +
                           ", not the account holder. It is a hint, never proof: do not act on their behalf "
                           "or share the account holder's private things because of it.");
}

bool VoiceSessionService::answerOffer(Session& session, const std::string& userText)
{
  const auto offer = std::exchange(session.offer, std::nullopt);
  if (!offer || std::chrono::steady_clock::now() - offer->at > kOfferAnswerWindow)
    return false;
  const OfferReply reply = offerReplyOf(userText, session.lang);
  if (reply == OfferReply::Other)
    return false;
  if (reply == OfferReply::Accept &&
      !role_access::hasAppAction(userRoleFromString(session.role), role_access::AppAction::ShowCamera))
    return false;
  const bool en = session.lang == VoiceLang::En;
  std::string line;
  if (reply == OfferReply::Accept) {
    Json::Value arguments(Json::objectValue);
    arguments["camera"] = offer->camera;
    rememberAction(session, {.name = "app.show_camera", .arguments = compactJson(arguments)});
    session.history.addEvent("The app is showing the " + offer->camera + " camera.");
    line = en ? "Here it is." : "Aquí la tienes.";
  }
  else {
    line = en ? "Okay." : "Vale.";
  }
  const bool audible = speak(session, line).audible;
  if (audible || reply == OfferReply::Accept)
    session.history.addAssistant(line);
  else
    session.history.rollbackUser();
  session.speaking = false;
  if (session.history.trim())
    primeLlm(session);
  return true;
}

ChatRequest VoiceSessionService::turnRequest(Session& session)
{
  ChatRequest req;
  req.clientActions = true;
  req.messages = session.history.request();
  req.userId = session.userId;
  req.role = userRoleFromString(session.role);
  req.lang = langCode(session.lang);
  req.sessionId = session.callId;
  return req;
}

void VoiceSessionService::primeLlm(Session& session)
{
  applyNotes(session);
  ChatRequest req = turnRequest(session);
  req.prefillOnly = true;
  std::stop_token stop;
  {
    std::scoped_lock lock(session.turnMutex);
    if (!session.active.load())
      return;
    stop = session.callStop.get_token();
  }
  if (session.primeThread.joinable())
    session.primeThread.join();
  session.primeThread = std::thread([this, req = std::move(req), stop] {
    try {
      llm_.chatStream({.request = req,
                       .onToken = [](const std::string&, bool) {},
                       .stats = nullptr,
                       .cancellation = stop,
                       .onAction = {}});
    }
    catch (const std::exception& e) {
      LOG_DEBUG << "Voice: LLM priming skipped: " << e.what();
    }
  });
}

void VoiceSessionService::rememberAction(Session& session, const ClientAction& action)
{
  const int64_t id = ++session.actionSeq;
  {
    std::scoped_lock lock(session.noticeMutex);
    session.sentActions.emplace_back(id, action.name);
    if (session.sentActions.size() > kMaxRememberedActions)
      session.sentActions.pop_front();
  }
  argus::voice::v1::ServerFrame frame;
  frame.mutable_action()->set_id(id);
  frame.mutable_action()->set_name(action.name);
  frame.mutable_action()->set_arguments(action.arguments);
  LOG_INFO << "Voice: client action " << id << " " << action.name;
  sendFrame(session, std::move(frame));
}

VoiceSessionService::SpeakOutcome VoiceSessionService::speak(Session& session,
                                                             const std::string& text)
{
  SpeakOutcome outcome;
  if (text.find_first_not_of(" \n\t") == std::string::npos || !session.sink ||
      !session.sink->connected())
    return outcome;
  std::stop_token cancellation;
  {
    std::scoped_lock lock(session.turnMutex);
    if (!session.active.load() || session.interrupt.load()) {
      outcome.interrupted = true;
      return outcome;
    }
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
  const auto markAudible = [&outcome] {
    if (outcome.audible)
      return;
    outcome.audible = true;
    outcome.firstAudioAt = std::chrono::steady_clock::now();
  };
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
      if (resampled.empty())
        return;
      argus::voice::v1::ServerFrame chunkFrame;
      chunkFrame.mutable_tts_chunk()->set_pcm(
          reinterpret_cast<const char*>(resampled.data()),
          static_cast<size_t>(resampled.size()) * sizeof(int16_t));
      if (session.duplex) {
        if (!sendDuplexChunk(session, std::move(chunkFrame)))
          return;
      }
      else {
        sendFrame(session, std::move(chunkFrame));
      }
      markAudible();
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
    const bool delivered = sendDuplexAssistant(session, std::move(assistantFrame));
    outcome.interrupted = !delivered || session.interrupt.load();
    return outcome;
  }
  sendFrame(session, std::move(assistantFrame));
  outcome.interrupted = session.interrupt.load();
  return outcome;
}

void VoiceSessionService::skip(VoiceSessionSink& sink)
{
  const auto session = sessionOf(sink);
  if (!session)
    return;
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

void VoiceSessionService::mute(VoiceSessionSink& sink, bool muted)
{
  const auto session = sessionOf(sink);
  if (!session)
    return;
  if (session->muted.exchange(muted) == muted)
    return;
  LOG_INFO << "Voice: microphone " << (muted ? "muted" : "unmuted");
  if (muted) {
    {
      std::scoped_lock lock(session->pcmMutex);
      session->pcmRing.clear();
    }
    session->vadResetPending.store(true);
  }
  session->pcmCv.notify_one();
}

void VoiceSessionService::context(VoiceSessionSink& sink,
                                  const argus::voice::v1::VoiceContext& context)
{
  const auto session = sessionOf(sink);
  if (!session)
    return;
  {
    std::scoped_lock lock(session->noticeMutex);
    switch (context.kind()) {
      case argus::voice::v1::VOICE_CONTEXT_CAMERA_EVENT: {
        const std::string camera = sanitizedLine(context.camera(), kMaxCameraChars);
        if (camera.empty())
          return;
        session->pendingCamera = CameraNotice{.camera = camera,
                                              .summary = sanitizedLine(context.text(), kMaxSummaryChars),
                                              .at = std::chrono::steady_clock::now()};
        break;
      }
      case argus::voice::v1::VOICE_CONTEXT_SITUATION:
        session->pendingSituation = sanitizedBlock(context.text(), kMaxSituationChars);
        break;
      default: {
        const std::string note = sanitizedLine(context.text(), kMaxNoteChars);
        if (note.empty())
          return;
        session->pendingNotes.push_back(note);
        if (session->pendingNotes.size() > kMaxNotes)
          session->pendingNotes.erase(session->pendingNotes.begin());
        break;
      }
    }
  }
  session->pcmCv.notify_one();
}

void VoiceSessionService::actionResult(VoiceSessionSink& sink,
                                       const argus::voice::v1::VoiceActionResult& result)
{
  const auto session = sessionOf(sink);
  if (!session)
    return;
  {
    std::scoped_lock lock(session->noticeMutex);
    const auto sent = std::ranges::find(session->sentActions, result.id(),
                                        &std::pair<int64_t, std::string>::first);
    if (sent == session->sentActions.end()) {
      LOG_INFO << "Voice: result for unknown client action " << result.id();
      return;
    }
    LOG_INFO << "Voice: client action " << result.id() << " " << sent->second
             << (result.ok() ? " done" : " failed");
    if (result.ok())
      return;
    session->pendingFailures.push_back(
        {.name = sent->second,
         .detail = sanitizedLine(result.detail(), kMaxDetailChars),
         .at = std::chrono::steady_clock::now()});
    if (session->pendingFailures.size() > kMaxPendingFailures)
      session->pendingFailures.pop_front();
  }
  session->pcmCv.notify_one();
}

void VoiceSessionService::applyNotes(Session& session)
{
  std::vector<std::string> notes;
  std::optional<std::string> situation;
  {
    std::scoped_lock lock(session.noticeMutex);
    notes.swap(session.pendingNotes);
    situation = std::exchange(session.pendingSituation, std::nullopt);
  }
  for (const auto& note : notes)
    session.history.addNote(note);
  if (situation)
    session.history.setSituation(*situation);
}

std::optional<VoiceSessionService::Notice>
VoiceSessionService::takeNotice(Session& session)
{
  const auto now = std::chrono::steady_clock::now();
  std::scoped_lock lock(session.noticeMutex);
  while (!session.pendingFailures.empty()) {
    const ActionFailure failure = std::move(session.pendingFailures.front());
    session.pendingFailures.pop_front();
    if (now - failure.at > kFailureFreshFor)
      continue;
    const ActionFailureText text{.lang = session.lang, .name = failure.name, .detail = failure.detail};
    return Notice{.spoken = actionFailureLine(text), .event = actionFailureEvent(text), .camera = {}};
  }
  while (!session.pendingAnnouncements.empty()) {
    Announcement announcement = std::move(session.pendingAnnouncements.front());
    session.pendingAnnouncements.pop_front();
    if (now - announcement.at > kAnnouncementFreshFor)
      continue;
    session.lastNoticeAt = now;
    return Notice{.spoken = std::move(announcement.text), .event = {}, .camera = {}};
  }
  auto camera = std::exchange(session.pendingCamera, std::nullopt);
  if (!camera)
    return std::nullopt;
  if (now - camera->at > kNoticeFreshFor || now - session.lastNoticeAt < kNoticeSpacing)
    return std::nullopt;
  session.lastNoticeAt = now;
  LOG_INFO << "Voice: offering camera " << camera->camera;
  return Notice{.spoken = cameraOffer({.lang = session.lang, .camera = camera->camera, .summary = camera->summary}),
                .event = {},
                .camera = camera->camera};
}

void VoiceSessionService::deliverNotice(Session& session, const Notice& notice)
{
  {
    std::scoped_lock lock(session.turnMutex);
    if (!session.active.load())
      return;
    session.interrupt.store(false);
    session.turnStop = std::stop_source{};
  }
  applyNotes(session);
  session.history.addEvent(notice.event);
  if (speak(session, notice.spoken).audible) {
    session.history.addNotice(notice.spoken);
    if (!notice.camera.empty())
      session.offer = CameraOffer{.camera = notice.camera, .at = std::chrono::steady_clock::now()};
  }
  if (session.history.trim())
    primeLlm(session);
}

void VoiceSessionService::sendFrame(Session& session,
                                    argus::voice::v1::ServerFrame frame) const
{
  if (!session.sink || !session.sink->connected() || session.farewell.load())
    return;
  session.sink->sendServerFrame(std::move(frame));
}
