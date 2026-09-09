#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <identity/identity-client.hxx>
#include <memory>
#include <mutex>
#include <shared/enums.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/llm/remote/llm-remote.hxx>
#include <shared/services/stt/remote/stt-remote.hxx>
#include <shared/services/tts/remote/tts-remote.hxx>
#include <string>
#include <vector>

// Engine seams behind the voice session; argus-voice is remote-only.
class IVoiceStt
{
public:
  virtual ~IVoiceStt() = default;

  virtual std::string transcribe(const std::vector<float>& audioSamples,
                                 int32_t sampleRate) = 0;
  virtual bool setLanguage(const std::string& lang) = 0;
};

class IVoiceTts
{
public:
  virtual ~IVoiceTts() = default;

  virtual float defaultSpeed() const = 0;
  virtual int sampleRate() const = 0;
  virtual void synthesizeStream(const TtsRequest& req,
                                TtsChunkCallback onChunk) = 0;
};

class IVoiceLlm
{
public:
  virtual ~IVoiceLlm() = default;

  virtual void chatStream(const ChatRequest& req, TokenCallback onToken) = 0;
};

// Spoken-name write input: the role is the x-argus-role metadata value.
struct VoiceNameWrite
{
  int64_t userId{0};
  std::string role;
  std::string name;
};

// The typed spoken-name write: IdentityService.UpdateUser on the gateway.
class IVoiceIdentity
{
public:
  virtual ~IVoiceIdentity() = default;

  virtual void updateUserName(const VoiceNameWrite& write) = 0;
};

// IdentityClient-backed adapter over identity.target.
class GrpcVoiceIdentity final : public IVoiceIdentity
{
public:
  void updateUserName(const VoiceNameWrite& write) override;

private:
  std::shared_ptr<const IdentityClient> clientFor(const std::string& target);

  mutable std::mutex mutex_;
  std::string cachedTarget_;
  std::string cachedSecret_;
  std::shared_ptr<const IdentityClient> client_;
};

// IVoiceTts over the argus-tts internal wire.
class RemoteVoiceTts final : public IVoiceTts
{
public:
  float defaultSpeed() const override { return client_.defaultSpeed(); }

  int sampleRate() const override { return client_.sampleRate(); }

  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk) override
  {
    client_.synthesizeStream(req, std::move(onChunk));
  }

private:
  TtsClient client_;
};

// IVoiceStt over the argus-stt internal wire.
class RemoteVoiceStt final : public IVoiceStt
{
public:
  std::string transcribe(const std::vector<float>& audioSamples,
                         int32_t sampleRate) override;

  bool setLanguage(const std::string& lang) override;

private:
  std::shared_ptr<const SttHttpClient>
  clientFor(const SttRemoteConfig& config);

  mutable std::mutex mutex_;
  std::string cachedUrl_;
  int cachedTimeoutMs_{0};
  std::shared_ptr<const SttHttpClient> client_;
  std::string lang_;
};

// IVoiceLlm over the argus-llm internal wire.
class RemoteVoiceLlm final : public IVoiceLlm
{
public:
  void chatStream(const ChatRequest& req, TokenCallback onToken) override;

private:
  std::shared_ptr<const LlmHttpClient>
  clientFor(const LlmRemoteConfig& config);

  mutable std::mutex mutex_;
  std::string cachedUrl_;
  int cachedTimeoutMs_{0};
  std::shared_ptr<const LlmHttpClient> client_;
};

// Process-wide seam adapters.
IVoiceStt& voiceStt();
IVoiceTts& voiceTts();
IVoiceLlm& voiceLlm();
IVoiceIdentity& voiceIdentity();

// The engine set backing one voice session.
struct VoiceEngineSeam
{
  IVoiceStt& stt = voiceStt();
  IVoiceTts& tts = voiceTts();
  IVoiceLlm& llm = voiceLlm();
  IVoiceIdentity& identity = voiceIdentity();
};
