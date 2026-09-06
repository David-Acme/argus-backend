#include "voice-engine-seam.hxx"

#include <shared/services/tts/remote/tts-remote.hxx>

// Lazily resolved on the first call (after the boot config is loaded): the
// argus-stt HTTP adapter when stt.remote_url is set, the in-process
// singleton otherwise (Ruling BM).
IVoiceStt& voiceStt()
{
  static RemoteVoiceStt remoteAdapter;
  static SingletonVoiceStt localAdapter;
  if (SttRemoteConfig::resolve().enabled())
    return remoteAdapter;
  return localAdapter;
}

// Lazily resolved on the first call (after the boot config is loaded): the
// argus-tts HTTP adapter when tts.remote_url is set, the in-process
// singleton otherwise (Ruling BI).
IVoiceTts& voiceTts()
{
  static RemoteVoiceTts remoteAdapter;
  static SingletonVoiceTts localAdapter;
  if (TtsRemoteConfig::resolve().enabled())
    return remoteAdapter;
  return localAdapter;
}

// Lazily resolved on the first call (after the boot config is loaded): the
// argus-llm HTTP adapter when llm.remote_url is set, the in-process
// singleton otherwise (Ruling BU; the in-process engine keeps serving
// MemoryService until F4-6, Ruling BF).
IVoiceLlm& voiceLlm()
{
  static RemoteVoiceLlm remoteAdapter;
  static SingletonVoiceLlm localAdapter;
  if (LlmRemoteConfig::resolve().enabled())
    return remoteAdapter;
  return localAdapter;
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
  // Same accepted set as SttService::setLanguage: the voice session falls
  // back to the system default when the language cannot be applied, and the
  // legacy call never throws — so the gate is checked locally, no wire call.
  if (lang != "es" && lang != "en" && lang != "auto")
    return false;
  std::lock_guard<std::mutex> lock(mutex_);
  lang_ = lang;
  return true;
}
