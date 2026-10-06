#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sync/user-action-event.hxx>

#include <string>
#include <vector>

namespace
{
Json::Value wire(const std::string& table)
{
  Json::Value json;
  json["user_id"] = 7;
  json["record_id"] = 3;
  json["table_name"] = table;
  json["action"] = "update";
  json["old_data"] = Json::Value(Json::objectValue);
  json["new_data"] = Json::Value(Json::objectValue);
  json["ip_address"] = "10.0.0.2";
  return json;
}
}

TEST_CASE("an action event keeps the table it names and no module by default")
{
  const auto parsed = UserActionEvent::fromJson(wire("camera"));
  CHECK(parsed.has_value());
  const UserActionEvent event = parsed.value_or(UserActionEvent{});
  CHECK(event.tableName == TableName::Camera);
  CHECK(event.subject.empty());
  CHECK(event.module.empty());
  CHECK(event.tableText() == "camera");
  CHECK_FALSE(event.toJson().isMember("module"));
  CHECK(event.toJson()["table_name"].asString() == "camera");
}

TEST_CASE("a module action names the subject module and the module it concerns")
{
  Json::Value json = wire("module");
  json["module"] = "surveillance";
  const auto parsed = UserActionEvent::fromJson(json);
  CHECK(parsed.has_value());
  const UserActionEvent event = parsed.value_or(UserActionEvent{});
  CHECK(event.subject == "module");
  CHECK(event.module == "surveillance");
  CHECK(event.tableText() == "module");

  const Json::Value again = event.toJson();
  CHECK(again["table_name"].asString() == "module");
  CHECK(again["module"].asString() == "surveillance");
  const auto round = UserActionEvent::fromJson(again);
  CHECK(round.has_value());
  CHECK(round.value_or(UserActionEvent{}).subject == "module");
  CHECK(round.value_or(UserActionEvent{}).module == "surveillance");
}

TEST_CASE("an action event naming an unknown table or a malformed module is refused")
{
  CHECK_FALSE(UserActionEvent::fromJson(wire("not_a_table")).has_value());
  CHECK_FALSE(UserActionEvent::fromJson(wire("")).has_value());

  for (const std::string& bad : std::vector<std::string>{"", "Surveillance", "a b", "x/y", std::string(65, 'a')}) {
    Json::Value json = wire("module");
    json["module"] = bad;
    CHECK_FALSE(UserActionEvent::fromJson(json).has_value());
  }
  Json::Value typed = wire("module");
  typed["module"] = 5;
  CHECK_FALSE(UserActionEvent::fromJson(typed).has_value());
}

TEST_CASE("module ids are lowercase slugs")
{
  CHECK(user_action_event::isModuleId("core"));
  CHECK(user_action_event::isModuleId("my-module-2"));
  CHECK_FALSE(user_action_event::isModuleId(""));
  CHECK_FALSE(user_action_event::isModuleId("Core"));
  CHECK_FALSE(user_action_event::isModuleId("a_b"));
}
