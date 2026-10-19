#include <doctest/doctest.h>

#include <feature/llm/dtos/chat-dto.hxx>
#include <llm/llm-remote.hxx>

#include <json/json.h>

#include <sstream>
#include <string>

namespace
{
Json::Value parseJson(const std::string& text)
{
  Json::Value value;
  std::string errors;
  std::istringstream in(text);
  Json::parseFromStream(Json::CharReaderBuilder(), in, &value, &errors);
  return value;
}

ChatRequest conversationRequest(ConversationLegKind leg)
{
  ChatRequest request;
  request.messages = {{.role = "user", .content = "hola"}};
  request.userId = 7;
  request.role = UserRole::Resident;
  request.useConversation = true;
  request.conversationLeg = leg;
  return request;
}
}

TEST_CASE("the HTTP body carries the leg and the flag when the request has them")
{
  const Json::Value body = parseJson(chatRequestJson(conversationRequest(ConversationLegKind::Laptop)));
  CHECK(body["use_conversation"].asBool());
  CHECK(body["conversation_leg"].asString() == "laptop");
}

TEST_CASE("a request without the conversation fields sends neither of them")
{
  ChatRequest plain;
  plain.messages = {{.role = "user", .content = "hola"}};
  const Json::Value body = parseJson(chatRequestJson(plain));
  CHECK_FALSE(body.isMember("use_conversation"));
  CHECK_FALSE(body.isMember("conversation_leg"));
}

TEST_CASE("the HTTP round trip keeps the leg and the flag, as the gRPC path does")
{
  const std::string text = chatRequestJson(conversationRequest(ConversationLegKind::Camera));
  const ChatRequest back = ChatCompletionDto::fromJson(parseJson(text)).request();
  CHECK(back.useConversation);
  REQUIRE(back.conversationLeg.has_value());
  CHECK(*back.conversationLeg == ConversationLegKind::Camera);
}

TEST_CASE("an unknown leg name is no leg, so use_conversation with it is refused by the controller")
{
  const Json::Value body = parseJson(
      R"({"messages":[{"role":"user","content":"hola"}],"use_conversation":true,"conversation_leg":"tablet"})");
  const ChatRequest request = ChatCompletionDto::fromJson(body).request();
  CHECK(request.useConversation);
  CHECK_FALSE(request.conversationLeg.has_value());
}
