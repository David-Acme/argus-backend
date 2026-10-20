#include "watch-conversations.hxx"

#include <feature/watch/wake-words.hxx>

#include <trantor/utils/Logger.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <utility>

namespace
{

constexpr auto kReapEvery = std::chrono::seconds(1);
constexpr int64_t kKeepEndedSeconds = 60;

argus::voice::v1::VoiceLanguage protoLanguage(VoiceLang lang)
{
  switch (lang) {
    case VoiceLang::Es:
      return argus::voice::v1::VOICE_LANGUAGE_ES;
    case VoiceLang::En:
      return argus::voice::v1::VOICE_LANGUAGE_EN;
    case VoiceLang::System:
      break;
  }
  return argus::voice::v1::VOICE_LANGUAGE_SYSTEM;
}

VoiceLang langOfIdentity(const argus::voice::v1::VoiceIdentity& identity, VoiceLang fallback)
{
  switch (identity.language()) {
    case argus::voice::v1::VOICE_LANGUAGE_ES:
      return VoiceLang::Es;
    case argus::voice::v1::VOICE_LANGUAGE_EN:
      return VoiceLang::En;
    default:
      return fallback;
  }
}

}

std::string cameraSpeaker(VoiceLang lang, std::string_view cameraName)
{
  if (lang == VoiceLang::En)
    return "Someone at the " + std::string(cameraName) + " camera";
  return "Alguien en la cámara " + std::string(cameraName);
}

std::optional<CameraHop> chooseHop(const HopChoiceInput& input)
{
  for (const int64_t mate : input.roommates) {
    if (mate == input.from)
      continue;
    const auto info = input.cameraInfo ? input.cameraInfo(mate) : std::nullopt;
    if (!info || !info->talk)
      continue;
    if (input.listening && !input.listening(mate))
      continue;
    if (input.occupied && input.occupied(mate))
      continue;
    return CameraHop{.cameraId = mate, .name = info->name};
  }
  return std::nullopt;
}

ConversationManager::ConversationManager(ConversationManagerInput input)
    : input_(std::move(input)), stageTwo_(*input_.stt)
{
}

ConversationManager::~ConversationManager()
{
  shutdown();
}

void ConversationManager::start()
{
  reaper_ = std::jthread([this](const std::stop_token& stop) { reap(stop); });
}

void ConversationManager::shutdown()
{
  if (reaper_.joinable()) {
    reaper_.request_stop();
    reaper_.join();
  }
  std::map<std::string, std::shared_ptr<CameraConversation>> live;
  {
    std::scoped_lock lock(mutex_);
    live.swap(live_);
  }
  live.clear();
}

std::shared_ptr<CameraConversation> ConversationManager::onCamera(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  for (const auto& [id, conversation] : live_)
    if (!conversation->closed() && conversation->cameraId() == cameraId)
      return conversation;
  return nullptr;
}

size_t ConversationManager::liveFor(int64_t userId) const
{
  std::scoped_lock lock(mutex_);
  return static_cast<size_t>(std::ranges::count_if(live_, [userId](const auto& entry) {
    return !entry.second->closed() && entry.second->userId() == userId;
  }));
}

bool ConversationManager::occupied(int64_t cameraId) const
{
  return onCamera(cameraId) != nullptr;
}

std::shared_ptr<CameraConversation> ConversationManager::live(const std::string& conversationId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = live_.find(conversationId);
  return found == live_.end() ? nullptr : found->second;
}

void ConversationManager::onWake(const ConfirmedWake& wake)
{
  if (occupied(wake.cameraId)) {
    LOG_INFO << "Voice watch: camera " << wake.cameraId << " already holds a conversation";
    return;
  }
  ConversationOpening opening;
  opening.identity.set_language(protoLanguage(input_.lang));
  if (input_.resolve) {
    const auto resolved = input_.resolve(wake);
    if (!resolved) {
      LOG_INFO << "Voice watch: the wake on camera " << wake.cameraId << " opens no conversation";
      return;
    }
    opening = *resolved;
  }
  argus::voice::v1::VoiceStart start;
  *start.mutable_identity() = opening.identity;
  start.set_mode(argus::voice::v1::VOICE_MODE_HALF_DUPLEX);
  const std::string id = mintConversationId();
  const VoiceLang lang = langOfIdentity(opening.identity, input_.lang);
  auto conversation = std::make_shared<CameraConversation>(CameraConversationInput{
      .id = id,
      .cameraId = wake.cameraId,
      .cameraName = wake.cameraName,
      .start = start,
      .audience = opening.audience,
      .surface = opening.surface,
      .sessions = input_.sessions,
      .camera = input_.camera,
      .echo = input_.echo,
      .tts = input_.tts,
      .config = input_.config,
      .lang = lang,
      .notes = opening.notes,
      .onUpdate = input_.onUpdate,
      .nextCamera = [this](int64_t from) -> std::optional<CameraHop> {
        const std::vector<int64_t> mates = input_.rooms ? input_.rooms->roommates(from) : std::vector<int64_t>{};
        return chooseHop({.from = from,
                          .roommates = mates,
                          .cameraInfo = input_.cameraInfo,
                          .listening = input_.listening,
                          .occupied = [this](int64_t camera) { return occupied(camera); }});
      },
      .onEnded = [this](const ConversationEnded& done) { ended(done); },
      .onUserText = [this, id](const std::string& text) { heard(id, text); },
      .askConsent = opening.audience == ConversationAudience::Visitor,
      .onWithheld = [this, id] { withheld(id); },
      .visitorPersonId = opening.strangerPersonId});
  {
    std::scoped_lock lock(mutex_);
    live_[id] = conversation;
    levels_[id] = std::make_shared<SpeakerLevels>(input_.config.gainAlpha);
  }
  conversation->open(wake.capture ? wake.capture->snapshot() : std::vector<float>{});
  if (opening.audience == ConversationAudience::Visitor && deliverReply(conversation))
    LOG_INFO << "Voice watch: a household reply waits to be said on camera " << wake.cameraId;
  const std::optional<WatchCameraInfo> info = input_.cameraInfo ? input_.cameraInfo(wake.cameraId) : std::nullopt;
  if (input_.aim && info && info->panTilt && !opening.faceSeen) {
    {
      std::scoped_lock lock(mutex_);
      aimed_[id] = wake.cameraId;
    }
    input_.aim({.cameraId = wake.cameraId, .conversationId = id, .mode = argus::camera::v1::AIM_MODE_FIND});
  }
}

void ConversationManager::onFrame(const EarFrame& frame)
{
  std::vector<std::pair<std::shared_ptr<CameraConversation>, std::shared_ptr<SpeakerLevels>>> targets;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& [id, conversation] : live_) {
      if (conversation->closed())
        continue;
      const auto levels = levels_.find(id);
      targets.emplace_back(conversation, levels == levels_.end() ? nullptr : levels->second);
    }
  }
  for (const auto& [conversation, levels] : targets) {
    if (conversation->cameraId() == frame.cameraId)
      conversation->feed(frame);
    const bool nearby = !input_.rooms || input_.rooms->sameEnvironment(conversation->cameraId(), frame.cameraId);
    if (levels && frame.speech && nearby)
      levels->observe({.cameraId = frame.cameraId, .snrDb = frame.snrDb});
  }
}

bool ConversationManager::onPlaybackFire(const EarFire& fire)
{
  const auto conversation = onCamera(fire.cameraId);
  if (!conversation || !input_.sameSpeaker)
    return false;
  StageTwoResult result;
  std::vector<float> samples;
  if (fire.capture) {
    static_cast<void>(fire.capture->wait(input_.config.postRoll + std::chrono::milliseconds(1000)));
    samples = fire.capture->snapshot();
  }
  if (fire.transcript)
    result = judgeWake(*fire.transcript, input_.config.wakeMaxDistance);
  else
    result = stageTwo_.confirm({.samples = samples,
                                .language = voiceLangToString(input_.lang),
                                .maxDistance = input_.config.wakeMaxDistance});
  if (!result.accepted)
    return false;
  if (!input_.sameSpeaker(conversation->userId(), samples))
    return false;
  conversation->bargeIn(samples);
  return true;
}

std::vector<ConversationSnapshot> ConversationManager::list(int64_t userId) const
{
  std::vector<ConversationSnapshot> out;
  if (userId <= 0)
    return out;
  std::vector<std::shared_ptr<CameraConversation>> live;
  {
    std::scoped_lock lock(mutex_);
    for (const auto& [id, conversation] : live_)
      if (!conversation->closed() && conversation->userId() == userId)
        live.push_back(conversation);
    for (const auto& [id, snapshot] : finished_)
      if (snapshot.userId == userId && !live_.contains(id))
        out.push_back(snapshot);
  }
  for (const auto& conversation : live)
    out.push_back(conversation->snapshot());
  return out;
}

std::optional<ConversationSnapshot> ConversationManager::find(const std::string& conversationId) const
{
  if (const auto conversation = live(conversationId); conversation && !conversation->closed())
    return conversation->snapshot();
  std::scoped_lock lock(mutex_);
  const auto found = finished_.find(conversationId);
  if (found == finished_.end())
    return std::nullopt;
  return found->second;
}

bool ConversationManager::end(int64_t userId, const std::string& conversationId)
{
  if (const auto conversation = live(conversationId); conversation && !conversation->closed()) {
    if (conversation->userId() != userId)
      return false;
    conversation->requestEnd(ConversationEnd::Closing);
    return true;
  }
  ConversationSnapshot snapshot;
  {
    std::scoped_lock lock(mutex_);
    const auto found = finished_.find(conversationId);
    if (found == finished_.end() || found->second.userId != userId)
      return false;
    found->second.state = ConversationState::Ended;
    found->second.resumableUntil = 0;
    found->second.updatedAt = epochSeconds();
    snapshot = found->second;
  }
  if (input_.onUpdate)
    input_.onUpdate(snapshot);
  return true;
}

std::map<int64_t, float> ConversationManager::speakerLevels(const std::string& conversationId) const
{
  std::scoped_lock lock(mutex_);
  const auto found = levels_.find(conversationId);
  if (found == levels_.end() || !found->second)
    return {};
  return found->second->snapshot();
}

void ConversationManager::forget(const std::string& conversationId)
{
  std::scoped_lock lock(mutex_);
  finished_.erase(conversationId);
  carries_.erase(conversationId);
}

bool ConversationManager::bridge(int64_t userId, const std::string& conversationId)
{
  if (userId <= 0)
    return false;
  if (const auto conversation = live(conversationId); conversation && !conversation->closed()) {
    if (conversation->userId() != userId)
      return false;
    conversation->markHandoff(ConversationHandoff::Bridging);
    return true;
  }
  ConversationSnapshot snapshot;
  {
    std::scoped_lock lock(mutex_);
    const auto found = finished_.find(conversationId);
    if (found == finished_.end() || found->second.userId != userId ||
        found->second.state != ConversationState::Resumable || epochSeconds() > found->second.resumableUntil ||
        !carries_.contains(conversationId))
      return false;
    found->second.handoff = ConversationHandoff::Bridging;
    found->second.updatedAt = epochSeconds();
    snapshot = found->second;
  }
  if (input_.onUpdate)
    input_.onUpdate(snapshot);
  return true;
}

std::optional<VoiceSessionCarry> ConversationManager::claim(int64_t userId, const std::string& conversationId)
{
  if (userId <= 0)
    return std::nullopt;
  if (const auto conversation = live(conversationId); conversation && !conversation->closed()) {
    if (conversation->userId() != userId)
      return std::nullopt;
    return conversation->handOver(std::chrono::milliseconds(5000));
  }
  std::optional<VoiceSessionCarry> carry;
  ConversationSnapshot snapshot;
  {
    std::scoped_lock lock(mutex_);
    const auto found = finished_.find(conversationId);
    const auto kept = carries_.find(conversationId);
    if (found == finished_.end() || kept == carries_.end() || found->second.userId != userId ||
        found->second.state != ConversationState::Resumable || epochSeconds() > found->second.resumableUntil)
      return std::nullopt;
    carry = std::move(kept->second);
    carries_.erase(kept);
    found->second.state = ConversationState::Ended;
    found->second.handoff = ConversationHandoff::Device;
    found->second.endReason = ConversationEnd::Handoff;
    found->second.resumableUntil = 0;
    found->second.updatedAt = epochSeconds();
    snapshot = found->second;
  }
  if (input_.onUpdate)
    input_.onUpdate(snapshot);
  return carry;
}

void ConversationManager::heard(const std::string& conversationId, const std::string& text)
{
  const auto conversation = live(conversationId);
  if (!conversation)
    return;
  if (conversation->userId() > 0 && input_.locate) {
    const std::map<int64_t, float> levels = speakerLevels(conversationId);
    int64_t cameraId = conversation->cameraId();
    float snrDb = 0.0F;
    if (const auto loudest = std::ranges::max_element(levels, {}, [](const auto& entry) { return entry.second; });
        loudest != levels.end()) {
      cameraId = loudest->first;
      snrDb = loudest->second;
    }
    input_.locate({.userId = conversation->userId(),
                   .cameraId = cameraId,
                   .snrDb = snrDb,
                   .voiceprint = false,
                   .atMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count()});
  }
  if (conversation->userId() > 0 && handoffRequest(text)) {
    LOG_INFO << "Voice watch: conversation " << conversationId << " is asked onto the phone";
    conversation->requestEnd(ConversationEnd::Handoff);
    return;
  }
  if (!input_.panic || !panicPhrase(text))
    return;
  const ConversationSnapshot snapshot = conversation->snapshot();
  LOG_WARN << "Voice watch: panic heard on camera " << snapshot.cameraId;
  input_.panic({.cameraId = snapshot.cameraId,
                .userId = snapshot.userId,
                .speaker = cameraSpeaker(input_.lang, snapshot.cameraName),
                .saidAt = epochSeconds(),
                .conversationId = conversationId,
                .duress = false});
}

void ConversationManager::withheld(const std::string& conversationId)
{
  const auto conversation = live(conversationId);
  if (!conversation || conversation->userId() > 0 || !input_.panic)
    return;
  const ConversationSnapshot snapshot = conversation->snapshot();
  LOG_WARN << "Voice watch: an alert is raised from camera " << snapshot.cameraId;
  input_.panic({.cameraId = snapshot.cameraId,
                .userId = 0,
                .speaker = cameraSpeaker(input_.lang, snapshot.cameraName),
                .saidAt = epochSeconds(),
                .conversationId = conversationId,
                .duress = true});
}

void ConversationManager::ended(const ConversationEnded& ended)
{
  std::optional<int64_t> aimedCamera;
  {
    std::scoped_lock lock(mutex_);
    finished_[ended.snapshot.conversationId] = ended.snapshot;
    if (ended.carry && ended.snapshot.userId > 0 && ended.snapshot.state == ConversationState::Resumable)
      carries_[ended.snapshot.conversationId] = *ended.carry;
    if (const auto found = aimed_.find(ended.snapshot.conversationId); found != aimed_.end()) {
      aimedCamera = found->second;
      aimed_.erase(found);
    }
  }
  if (aimedCamera && input_.aim)
    input_.aim({.cameraId = *aimedCamera,
                .conversationId = ended.snapshot.conversationId,
                .mode = argus::camera::v1::AIM_MODE_RELEASE});
  if (input_.onEnded)
    input_.onEnded(ended);
}

void ConversationManager::reap(const std::stop_token& stop)
{
  std::mutex waitMutex;
  std::condition_variable_any wake;
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(waitMutex);
      static_cast<void>(wake.wait_for(lock, stop, std::chrono::milliseconds(kReapEvery), [] { return false; }));
    }
    std::vector<std::shared_ptr<CameraConversation>> retired;
    std::vector<ConversationSnapshot> expired;
    const int64_t now = epochSeconds();
    {
      std::scoped_lock lock(mutex_);
      for (auto it = live_.begin(); it != live_.end();) {
        if (it->second->closed()) {
          retired.push_back(std::move(it->second));
          it = live_.erase(it);
        }
        else {
          ++it;
        }
      }
      for (auto it = finished_.begin(); it != finished_.end();) {
        ConversationSnapshot& snapshot = it->second;
        if (snapshot.state == ConversationState::Resumable && now >= snapshot.resumableUntil) {
          snapshot.state = ConversationState::Ended;
          snapshot.endReason = ConversationEnd::Expired;
          snapshot.resumableUntil = 0;
          snapshot.updatedAt = now;
          expired.push_back(snapshot);
        }
        if (snapshot.state == ConversationState::Ended)
          carries_.erase(it->first);
        if (snapshot.state == ConversationState::Ended && now - snapshot.updatedAt > kKeepEndedSeconds) {
          levels_.erase(it->first);
          it = finished_.erase(it);
        }
        else {
          ++it;
        }
      }
    }
    for (const ConversationSnapshot& snapshot : expired)
      if (input_.onUpdate)
        input_.onUpdate(snapshot);
    retired.clear();
  }
}

bool ConversationManager::deliverReply(const std::shared_ptr<CameraConversation>& conversation)
{
  if (!input_.replies || !conversation || conversation->closed())
    return false;
  const ConversationSnapshot snapshot = conversation->snapshot();
  if (snapshot.audience != ConversationAudience::Visitor)
    return false;
  const auto pending = input_.replies->take(
      {.cameraId = snapshot.cameraId, .personId = snapshot.visitorPersonId, .now = epochSeconds()});
  if (!pending)
    return false;
  conversation->say(visitorReplyLine(input_.lang, pending->text));
  return true;
}

VisitorReplyResult ConversationManager::replyToVisitor(const VisitorReplyInput& input)
{
  if (!input_.replies)
    return {.outcome = VisitorReplyOutcome::NotFound, .replyUntil = 0, .cameraId = 0};
  VisitorReplyResult result = input_.replies->reply(input);
  if (result.outcome != VisitorReplyOutcome::Queued)
    return result;
  LOG_INFO << "Voice watch: a household reply to a visitor is kept for camera " << result.cameraId;
  if (deliverReply(onCamera(result.cameraId)))
    result.outcome = VisitorReplyOutcome::Said;
  return result;
}
