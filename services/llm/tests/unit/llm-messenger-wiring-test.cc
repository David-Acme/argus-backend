#include <doctest/doctest.h>

#include <feature/messenger/services/messenger-wiring.hxx>

TEST_CASE("the composed relay tool is the core message tool, gated by messages.relay")
{
  const auto wiring = composeMessenger({.store = nullptr,
                                       .notifications = nullptr,
                                       .identity = nullptr,
                                       .conversations = nullptr,
                                       .settings = MessengerSettings{}});
  CHECK(wiring.tool.spec.name == "message.relay");
  CHECK(wiring.tool.spec.capability == "messages.relay");
  CHECK(wiring.tool.spec.module.empty());
  CHECK(static_cast<bool>(wiring.tool.handler));
}

TEST_CASE("without an identity there is no household roster, so the composed tool refuses every sender")
{
  const auto wiring = composeMessenger({.store = nullptr,
                                       .notifications = nullptr,
                                       .identity = nullptr,
                                       .conversations = nullptr,
                                       .settings = MessengerSettings{}});
  tools::ToolCall call;
  call.name = "message.relay";
  call.arguments["recipient"] = "Ana Lopez";
  call.arguments["message"] = "venga a comer";
  call.context.userId = 1;
  call.context.lang = "es";
  call.context.now = 1000;
  const auto result = wiring.tool.handler(call);
  CHECK_FALSE(result.ok);
  CHECK(result.code == "sender_unknown");
}
