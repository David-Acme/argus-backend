#pragma once

#include <argus/voice/v1/voice.pb.h>
#include <identity/identity-client.hxx>
#include <memory>
#include <mutex>
#include <llm/llm-service.hxx>
#include <llm/llm-remote.hxx>
#include <stt/stt-remote.hxx>
#include <tts/tts-remote.hxx>
#include <stop_token>
#include <string>
#include <vector>

struct VoiceTranscribeInput
{
  const std::vector<float>& samples;
  int32_t sampleRate{0};
  std::string language;
};

class IVoiceStt
{
public:
  virtual ~IVoiceStt() = default;

  virtual std::string transcribe(const VoiceTranscribeInput& input) = 0;
};

class IVoiceTts
{
public:
  virtual ~IVoiceTts() = default;

  virtual float defaultSpeed(std::stop_token cancellation = {}) const = 0;
  virtual int sampleRate(std::stop_token cancellation = {}) const = 0;
  void synthesizeStream(const TtsRequest& req, TtsChunkCallback onChunk)
  {
    synthesizeStream({.request = req, .onChunk = std::move(onChunk), .cancellation = {}});
  }
  virtual void synthesizeStream(TtsRemoteStreamInput input) = 0;
};

class IVoiceLlm
{
public:
  virtual ~IVoiceLlm() = default;

  virtual void chatStream(LlmStreamInput input) = 0;
};

struct VoiceNameWrite
{
  int64_t userId{0};
  std::string role;
  std::string name;
};

class IVoiceIdentity
{
public:
  virtual ~IVoiceIdentity() = default;

  virtual void updateUserName(const VoiceNameWrite& write) = 0;
};

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

class RemoteVoiceTts final : public IVoiceTts
{
public:
  using IVoiceTts::synthesizeStream;

  float defaultSpeed(std::stop_token cancellation = {}) const override
  {
    return client_.defaultSpeed(cancellation);
  }

  int sampleRate(std::stop_token cancellation = {}) const override
  {
    return client_.sampleRate(cancellation);
  }

  void synthesizeStream(TtsRemoteStreamInput input) override
  {
    client_.synthesizeStream(std::move(input));
  }

private:
  TtsClient client_;
};

class RemoteVoiceStt final : public IVoiceStt
{
public:
  std::string transcribe(const VoiceTranscribeInput& input) override;

  static bool supportsLanguage(const std::string& lang);

private:
  SttClient client_;
};

class RemoteVoiceLlm final : public IVoiceLlm
{
public:
  void chatStream(LlmStreamInput input) override;

private:
  std::shared_ptr<const LlmClient>
  clientFor(const LlmRemoteConfig& config);

  mutable std::mutex mutex_;
  std::string cachedUrl_;
  int cachedTimeoutMs_{0};
  std::shared_ptr<const LlmClient> client_;
};

IVoiceStt& voiceStt();
IVoiceTts& voiceTts();
IVoiceLlm& voiceLlm();
IVoiceIdentity& voiceIdentity();

struct VoiceEngineSeam
{
  IVoiceStt& stt = voiceStt();
  IVoiceTts& tts = voiceTts();
  IVoiceLlm& llm = voiceLlm();
  IVoiceIdentity& identity = voiceIdentity();
};
