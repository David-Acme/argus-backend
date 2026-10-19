#include <doctest/doctest.h>

#include <json/value.h>
#include <optional>
#include <string>
#include <sync/action-journal-fields.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action-event.hxx>
#include <vector>

namespace
{
UserActionEvent smuggled(TableName table)
{
  Json::Value content(Json::objectValue);
  content["title"] = "Cumpleaños de Ana";
  content["description"] = "comprar la torta";
  content["location"] = "casa";
  content["scheduledAt"] = 1790000000;
  Json::Value before(Json::objectValue);
  before["title"] = "Cumple";
  before["description"] = "antes";
  return {.userId = 7,
          .recordId = 11,
          .tableName = table,
          .action = UserAction::Update,
          .oldData = before,
          .newData = content,
          .ipAddress = "",
          .module = "",
          .subject = ""};
}

std::vector<std::string> fieldsOf(const Json::Value& data)
{
  return data.isObject() ? data.getMemberNames() : std::vector<std::string>{};
}
}

TEST_CASE("a personal table journals who, kind, table and record only, whatever the producer sent")
{
  for (const TableName table :
       {TableName::Reminder, TableName::ReminderDetail, TableName::ProjectMember,
        TableName::CalendarEventShare}) {
    INFO(tableNameToString(table));
    UserActionEvent event = smuggled(table);
    action_journal::restrict(event);
    CHECK(event.oldData.isNull());
    CHECK(event.newData.isNull());
    CHECK(event.userId == 7);
    CHECK(event.recordId == 11);
    CHECK(event.action == UserAction::Update);
    CHECK(action_journal::journalData(table, std::string("Cumpleaños")).isNull());
  }
}

TEST_CASE("a shareable table keeps at most its title, and never the before")
{
  for (const TableName table :
       {TableName::Project, TableName::ProjectTask, TableName::CalendarEvent}) {
    INFO(tableNameToString(table));
    UserActionEvent event = smuggled(table);
    action_journal::restrict(event);
    CHECK(event.oldData.isNull());
    CHECK(fieldsOf(event.newData) == std::vector<std::string>{"title"});
    CHECK(event.newData["title"].asString() == "Cumpleaños de Ana");

    CHECK(action_journal::journalData(table, std::nullopt).isNull());
    CHECK(action_journal::journalData(table, std::string()).isNull());
    CHECK(fieldsOf(action_journal::journalData(table, std::string("Casa"))) ==
          std::vector<std::string>{"title"});
  }
}

TEST_CASE("a title that is not text, or content without a title, reaches nothing")
{
  UserActionEvent numeric = smuggled(TableName::Project);
  numeric.newData["title"] = 42;
  action_journal::restrict(numeric);
  CHECK(numeric.newData.isNull());

  UserActionEvent listed = smuggled(TableName::CalendarEvent);
  listed.newData = Json::Value(Json::arrayValue);
  listed.newData.append("title");
  action_journal::restrict(listed);
  CHECK(listed.newData.isNull());
}

TEST_CASE("tables outside the productivity set and module events are left to their producers")
{
  UserActionEvent user = smuggled(TableName::User);
  action_journal::restrict(user);
  CHECK(user.newData["description"].asString() == "comprar la torta");
  CHECK(user.oldData.isObject());

  UserActionEvent module = smuggled(TableName::Reminder);
  module.subject = std::string(user_action_event::kModuleSubject);
  action_journal::restrict(module);
  CHECK(module.newData.isObject());
}
