#pragma once

#include <drogon/HttpController.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>
#include <feature/llm/services/lfm-adapter.hxx>
#include <llm/llm-service.hxx>
#include <runtime/stream-slots.hxx>
#include <shared/vocabulary/tool-contracts.hxx>
#include <shared/vocabulary/utterance-screen.hxx>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct LlmChatOutcome
{
  std::string text;
  std::string rawReply{};
  std::string contextBlock{};
  std::string speech{};
  std::string act{};
  std::string guardVerdict{};
  std::string firstGuardVerdict{};
  std::string firstRejected{};
  std::string softVerdict{};
  std::vector<SpeechAttempt> attempts{};
  int hops{0};
  size_t toolCalls{0};
  bool softRelease{false};
  std::vector<tools::ToolCall> attempted{};
  int64_t generateMs{0};
  int64_t toolMs{0};
};

inline constexpr std::string_view kIdentityCaller = "voice";

[[nodiscard]] ChatRequest boundToCaller(ChatRequest request, std::string_view caller);

struct CallerCredential
{
  std::string_view presented;
  std::string_view expected;
};

[[nodiscard]] ChatRequest boundToCredential(ChatRequest request, const CallerCredential& credential);

struct ScreenedRequest
{
  ChatRequest request;
  bool withheld{false};
};

[[nodiscard]] ScreenedRequest screenTurn(const turn::UtteranceScreen* screen, ChatRequest request);

class LlmController : public drogon::HttpController<LlmController, false>
{
public:
  METHOD_LIST_BEGIN
  ADD_METHOD_TO(LlmController::chat, "/llm/v1/chat", drogon::Post);
  ADD_METHOD_TO(LlmController::chatStream, "/llm/v1/chat-stream", drogon::Post);
  ADD_METHOD_TO(LlmController::engine, "/llm/v1/config", drogon::Get);
  ADD_METHOD_TO(LlmController::screenUtterance, "/llm/v1/screen", drogon::Post);
  METHOD_LIST_END

  LlmController();

  void initEngine();
  void shutdownEngine();
  bool isEngineLoaded();

  LlmService& service() { return service_; }

  StreamSlots& streams() { return streams_; }

  void setIdentityCredential(std::string credential);

  LfmAdapter& adapter() { return adapter_; }

  void useScreen(const turn::UtteranceScreen& screen);

  [[nodiscard]] std::string screenText(const ChatRequest& request) const;

  LlmChatOutcome chatSync(const ChatRequest& request);
  void chatStreamSync(const LlmStreamInput& input);

  drogon::Task<drogon::HttpResponsePtr> chat(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> chatStream(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> engine(drogon::HttpRequestPtr req);
  drogon::Task<drogon::HttpResponsePtr> screenUtterance(drogon::HttpRequestPtr req);

private:
  LlmService service_;
  LfmAdapter adapter_;
  StreamSlots streams_;
  std::string identityCredential_;
  const turn::UtteranceScreen* screen_{nullptr};
};
