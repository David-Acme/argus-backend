#include "voice-engine-seam.hxx"

#include <algorithm>
#include <chrono>
#include <drogon/drogon.h>
#include <iterator>
#include <identity/identity-client.hxx>
#include <config/config-service.hxx>
#include <stdexcept>

namespace
{
constexpr int kIdentityTimeoutMs = 5000;
constexpr auto kTtsCapabilitiesTtl = std::chrono::seconds(10);
constexpr int kSpeakerTimeoutMs = 800;

std::string identityTarget()
{
  return ConfigService::getString("identity.target");
}
}

IVoiceStt& voiceStt()
{
  static RemoteVoiceStt adapter;
  return adapter;
}

IVoiceTts& voiceTts()
{
  static RemoteVoiceTts adapter;
  return adapter;
}

IVoiceLlm& voiceLlm()
{
  static RemoteVoiceLlm adapter;
  return adapter;
}

IVoiceIdentity& voiceIdentity()
{
  static GrpcVoiceIdentity adapter;
  return adapter;
}

IVoiceVad& voiceVad()
{
  static SileroVoiceVad adapter;
  return adapter;
}

IVoiceSpeaker& voiceSpeaker()
{
  static GrpcVoiceSpeaker adapter;
  return adapter;
}

std::shared_ptr<const VoiceprintClient>
GrpcVoiceSpeaker::clientFor(const std::string& target)
{
  const auto secret = ConfigService::getString("identity.rpc_secret");
  if (!client_ || target != cachedTarget_ || secret != cachedSecret_) {
    cachedTarget_ = target;
    cachedSecret_ = secret;
    client_ = std::make_shared<VoiceprintClient>(
        VoiceprintClientConfig{.target = target, .fleetSecret = secret});
  }
  return client_;
}

std::optional<VoiceSpeaker> GrpcVoiceSpeaker::identify(const VoiceSpeakerInput& input)
{
  const std::string target = identityTarget();
  if (target.empty() || input.samples.empty())
    return std::nullopt;
  std::shared_ptr<const VoiceprintClient> client;
  {
    std::scoped_lock lock(mutex_);
    client = clientFor(target);
  }
  std::vector<int16_t> pcm;
  pcm.reserve(input.samples.size());
  std::ranges::transform(input.samples, std::back_inserter(pcm), [](float sample) {
    return static_cast<int16_t>(std::clamp(sample, -1.0F, 1.0F) * 32767.0F);
  });
  const auto answer = client->identifyWithin(
      {.sample = {.samples = pcm, .sampleRate = input.sampleRate}, .timeoutMs = kSpeakerTimeoutMs});
  if (!answer || answer->outcome() != argus::identity::v1::VOICEPRINT_OK || !answer->matched() ||
      answer->user_id() <= 0)
    return std::nullopt;
  return VoiceSpeaker{.userId = answer->user_id(),
                      .name = answer->has_name() ? answer->name() : std::string(),
                      .score = answer->score()};
}

std::shared_ptr<const IdentityClient>
GrpcVoiceIdentity::clientFor(const std::string& target)
{
  const auto secret = ConfigService::getString("identity.rpc_secret");
  if (!client_ || target != cachedTarget_ || secret != cachedSecret_) {
    cachedTarget_ = target;
    cachedSecret_ = secret;
    client_ = std::make_shared<IdentityClient>(target, secret);
  }
  return client_;
}

RemoteVoiceTts::Capabilities RemoteVoiceTts::capabilities(const std::stop_token& cancellation) const
{
  const auto now = std::chrono::steady_clock::now();
  {
    std::scoped_lock lock(mutex_);
    if (cached_ && now - cached_->at < kTtsCapabilitiesTtl)
      return *cached_;
  }
  const Capabilities fresh{.speed = client_.defaultSpeed(cancellation),
                           .sampleRate = client_.sampleRate(cancellation),
                           .at = now};
  std::scoped_lock lock(mutex_);
  cached_ = fresh;
  return fresh;
}

float RemoteVoiceTts::defaultSpeed(std::stop_token cancellation) const
{
  return capabilities(cancellation).speed;
}

int RemoteVoiceTts::sampleRate(std::stop_token cancellation) const
{
  return capabilities(cancellation).sampleRate;
}

bool RemoteVoiceStt::supportsLanguage(const std::string& lang)
{
  return lang.empty() || lang == "es" || lang == "en" || lang == "auto";
}

std::string RemoteVoiceStt::transcribe(const VoiceTranscribeInput& input)
{
  if (!client_.remote())
    throw std::runtime_error("stt.remote_url is not configured");
  if (input.sampleRate != kWireSampleRate)
    throw std::runtime_error("argus-stt wire requires 16 kHz mono PCM");
  if (!supportsLanguage(input.language))
    throw std::invalid_argument("argus-stt does not support language " +
                                input.language);
  return client_.transcribe(input.samples, input.language);
}

std::shared_ptr<const LlmClient>
RemoteVoiceLlm::clientFor(const LlmRemoteConfig& config)
{
  if (!client_ || config.url != cachedUrl_ ||
      config.timeoutMs != cachedTimeoutMs_) {
    cachedUrl_ = config.url;
    cachedTimeoutMs_ = config.timeoutMs;
    client_ = std::make_shared<LlmClient>(config.url, config.timeoutMs);
  }
  return client_;
}

void RemoteVoiceLlm::chatStream(LlmStreamInput input)
{
  const LlmRemoteConfig config = LlmRemoteConfig::resolve();

  std::shared_ptr<const LlmClient> client;
  {
    std::scoped_lock lock(mutex_);
    client = clientFor(config);
  }
  if (!client->remote())
    throw std::runtime_error("llm.remote_url is not configured");
  client->chatStream(input);
}

void GrpcVoiceIdentity::updateUserName(const VoiceNameWrite& write)
{
  const std::string target = identityTarget();
  if (target.empty()) {
    LOG_WARN << "Voice: identity.target is not configured; spoken name not "
                "persisted";
    return;
  }

  std::shared_ptr<const IdentityClient> client;
  {
    std::scoped_lock lock(mutex_);
    client = clientFor(target);
  }

  UpdateUserNameInput input;
  input.userId = write.userId;
  input.role = write.role;
  input.name = write.name;
  if (!client->updateUserName(input))
    LOG_WARN << "Voice: identity UpdateUser failed for user=" << write.userId;
}
