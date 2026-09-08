#include "voice-engine-seam.hxx"

#include <chrono>
#include <drogon/drogon.h>
#include <identity/identity-client.hxx>
#include <shared/services/config-service/config-service.hxx>

namespace
{
constexpr int kIdentityTimeoutMs = 5000;

std::string identityTarget()
{
  return ConfigService::getString("identity.target");
}
} // namespace

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

std::shared_ptr<const SttHttpClient>
RemoteVoiceStt::clientFor(const SttRemoteConfig& config)
{
  if (!client_ || config.url != cachedUrl_ ||
      config.timeoutMs != cachedTimeoutMs_) {
    cachedUrl_ = config.url;
    cachedTimeoutMs_ = config.timeoutMs;
    client_ = std::make_shared<SttHttpClient>(config.url, config.timeoutMs);
  }
  return client_;
}

std::string RemoteVoiceStt::transcribe(const std::vector<float>& audioSamples,
                                       int32_t sampleRate)
{
  const SttRemoteConfig config = SttRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("stt.remote_url is not configured");
  if (sampleRate != kWireSampleRate)
    throw std::runtime_error("argus-stt wire requires 16 kHz mono PCM");

  std::shared_ptr<const SttHttpClient> client;
  std::string lang;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = clientFor(config);
    lang = lang_;
  }
  return client->transcribe(audioSamples, lang);
}

bool RemoteVoiceStt::setLanguage(const std::string& lang)
{
  if (lang != "es" && lang != "en" && lang != "auto")
    return false;
  std::lock_guard<std::mutex> lock(mutex_);
  lang_ = lang;
  return true;
}

std::shared_ptr<const LlmHttpClient>
RemoteVoiceLlm::clientFor(const LlmRemoteConfig& config)
{
  if (!client_ || config.url != cachedUrl_ ||
      config.timeoutMs != cachedTimeoutMs_) {
    cachedUrl_ = config.url;
    cachedTimeoutMs_ = config.timeoutMs;
    client_ = std::make_shared<LlmHttpClient>(config.url, config.timeoutMs);
  }
  return client_;
}

void RemoteVoiceLlm::chatStream(const ChatRequest& req, TokenCallback onToken)
{
  const LlmRemoteConfig config = LlmRemoteConfig::resolve();
  if (!config.enabled())
    throw std::runtime_error("llm.remote_url is not configured");

  std::shared_ptr<const LlmHttpClient> client;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    client = clientFor(config);
  }
  LlmStreamInput input;
  input.request = req;
  input.onToken = std::move(onToken);
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
    std::lock_guard<std::mutex> lock(mutex_);
    client = clientFor(target);
  }

  UpdateUserNameInput input;
  input.userId = write.userId;
  input.role = write.role;
  input.name = write.name;
  if (!client->updateUserName(input))
    LOG_WARN << "Voice: identity UpdateUser failed for user=" << write.userId;
}
