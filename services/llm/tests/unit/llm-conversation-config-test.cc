#include <doctest/doctest.h>

#include <config/llm-config.hxx>

TEST_CASE("the conversation defaults are the ruled values")
{
  const LlmConversationConfig defaults;
  CHECK(defaults.idleSeconds == 900);
  CHECK(defaults.retentionDays == 30);
  CHECK(defaults.switchBudgetMs == 100);
}

TEST_CASE("values inside their range pass through unchanged")
{
  const LlmConversationConfig inside{.idleSeconds = 900, .retentionDays = 30, .switchBudgetMs = 100};
  const auto bounded = LlmConfig::boundConversation(inside);
  CHECK(bounded.idleSeconds == 900);
  CHECK(bounded.retentionDays == 30);
  CHECK(bounded.switchBudgetMs == 100);
}

TEST_CASE("values outside their range are clamped to the nearest bound")
{
  const LlmConversationConfig low{.idleSeconds = 1, .retentionDays = 0, .switchBudgetMs = 0};
  const auto lowBounded = LlmConfig::boundConversation(low);
  CHECK(lowBounded.idleSeconds == 60);
  CHECK(lowBounded.retentionDays == 1);
  CHECK(lowBounded.switchBudgetMs == 1);

  const LlmConversationConfig high{.idleSeconds = 1000000, .retentionDays = 9999, .switchBudgetMs = 5000};
  const auto highBounded = LlmConfig::boundConversation(high);
  CHECK(highBounded.idleSeconds == 86400);
  CHECK(highBounded.retentionDays == 365);
  CHECK(highBounded.switchBudgetMs == 1000);
}
