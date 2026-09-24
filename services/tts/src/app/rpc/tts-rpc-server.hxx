#pragma once

#include <tts/tts-client.hxx>
#include <feature/synthesis/services/tts-service.hxx>
#include <tts/tts-wire.hxx>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct TtsRpcInput
{
  std::string address;
  std::vector<std::pair<std::string, std::string>> credentials;
  argus::tts::Capabilities capabilities;
  std::function<void(TtsStreamInput)> synthesize;
  int slots{1};
};

class TtsRpcServer
{
public:
  explicit TtsRpcServer(TtsRpcInput input);
  ~TtsRpcServer();
  int port() const;
  void shutdown();
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
