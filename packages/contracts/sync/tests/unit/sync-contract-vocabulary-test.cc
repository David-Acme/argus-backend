#include <doctest/doctest.h>

#include <sync/audit-log-priority.hxx>
#include <sync/sync-operation.hxx>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>

#include <cstddef>
#include <string>
#include <vector>

template <typename Enum>
struct CheckRoundTripInput
{
    const std::vector<Enum>& values;
    const std::vector<std::string>& names;
    std::string (*toString)(Enum);
    Enum (*fromString)(const std::string&);
};

template <typename Enum>
static void checkRoundTrip(const CheckRoundTripInput<Enum>& input)
{
    const auto& values = input.values;
    const auto& names = input.names;
    auto toString = input.toString;
    auto fromString = input.fromString;

    REQUIRE(values.size() == names.size());
    for (size_t i = 0; i < values.size(); ++i) {
        CHECK(toString(values[i]) == names[i]);
        CHECK(fromString(names[i]) == values[i]);
    }
}

TEST_CASE("sync operation strings round-trip and keep their wire numbers")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {SyncOperation::InitialInfo, SyncOperation::Synchronize,
                   SyncOperation::SynchronizeAuditLog,
                   SyncOperation::SynchronizeUserAuditLog, SyncOperation::Add,
                   SyncOperation::Delete, SyncOperation::Log,
                   SyncOperation::AuthContextChanged,
                   SyncOperation::CallIncoming, SyncOperation::CallCancel,
                   SyncOperation::ResponseUpdate, SyncOperation::Heartbeat,
                   SyncOperation::ModuleUpdate, SyncOperation::ContextUpdate,
                   SyncOperation::ResourceChanged, SyncOperation::WatchUpdate},
        .names = {"initial_info", "sync", "sync_audit_log",
                  "sync_user_audit_log", "add", "delete", "log",
                  "auth_context_changed", "call_incoming", "call_cancel",
                  "response_update", "heartbeat", "module_update",
                  "context_update", "resource_changed", "watch_update"},
        .toString = syncOperationToString,
        .fromString = syncOperationFromString});
    CHECK(static_cast<int>(SyncOperation::AuthContextChanged) == 7);
    CHECK(static_cast<int>(SyncOperation::CallIncoming) == 8);
    CHECK(static_cast<int>(SyncOperation::CallCancel) == 9);
    CHECK(static_cast<int>(SyncOperation::ResponseUpdate) == 10);
    CHECK(static_cast<int>(SyncOperation::Heartbeat) == 11);
    CHECK(static_cast<int>(SyncOperation::ModuleUpdate) == 12);
    CHECK(static_cast<int>(SyncOperation::ContextUpdate) == 13);
    CHECK(static_cast<int>(SyncOperation::ResourceChanged) == 14);
    CHECK(static_cast<int>(SyncOperation::WatchUpdate) == 15);
    CHECK(syncOperationFromString("unknown") == SyncOperation::Synchronize);
}

TEST_CASE("user action strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {UserAction::Create, UserAction::Read, UserAction::Update,
                   UserAction::Delete},
        .names = {"create", "read", "update", "delete"},
        .toString = userActionToString,
        .fromString = userActionFromString});
}

TEST_CASE("table name strings round-trip")
{
    const std::vector<TableName> values = {
        TableName::User,       TableName::UserInvitation, TableName::Person,
        TableName::Event,      TableName::Reminder,       TableName::ReminderDetail,
        TableName::CalendarEvent,   TableName::CalendarEventShare,
        TableName::Project,         TableName::ProjectMember,
        TableName::ProjectTask,     TableName::ContextNote,
        TableName::Camera,          TableName::CameraStream,
        TableName::Zone,            TableName::AuditLog,
        TableName::UserAuditLog,    TableName::Notification,
        TableName::NotificationToken, TableName::UserActionLog,
        TableName::RefreshToken,    TableName::FaceEmbedding,
        TableName::Memory,          TableName::ConversationMessage,
    };
    const std::vector<std::string> names = {
        "user",               "user_invitation",     "person",
        "event",              "reminder",            "reminder_detail",
        "calendar_event",     "calendar_event_share", "project",
        "project_member",     "project_task",        "context_note",
        "camera",             "camera_stream",       "zone",
        "audit_log",          "user_audit_log",      "notification",
        "notification_token", "user_action_log",     "refresh_token",
        "face_embedding",     "memory",              "conversation_message",
    };
    checkRoundTrip(CheckRoundTripInput{.values = values,
                                        .names = names,
                                        .toString = tableNameToString,
                                        .fromString = tableNameFromString});

    CHECK(tableNameToString(TableName::Memory) == "memory");
    CHECK(tableNameToString(TableName::PersonEvent) == "person_event");
}

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(userActionFromString("bogus") == UserAction::Create);
    CHECK(tableNameFromString("bogus") == TableName::User);
}

TEST_CASE("every table name keeps the number it was given")
{
    const std::vector<std::pair<TableName, int>> pinned = {
        {TableName::User, 0},              {TableName::UserInvitation, 1},
        {TableName::Person, 2},           {TableName::PersonEvent, 3},
        {TableName::Event, 4},            {TableName::Reminder, 5},
        {TableName::ReminderDetail, 6},   {TableName::CalendarEvent, 7},
        {TableName::CalendarEventShare, 8}, {TableName::Project, 9},
        {TableName::ProjectMember, 10},   {TableName::ProjectTask, 11},
        {TableName::ContextNote, 12},     {TableName::Camera, 13},
        {TableName::CameraStream, 14},    {TableName::Zone, 15},
        {TableName::AuditLog, 16},        {TableName::UserAuditLog, 17},
        {TableName::Notification, 18},    {TableName::NotificationToken, 19},
        {TableName::UserActionLog, 20},   {TableName::RefreshToken, 21},
        {TableName::FaceEmbedding, 22},   {TableName::Memory, 23},
        {TableName::ConversationMessage, 24},
    };
    for (const auto& [table, number] : pinned)
        CHECK(static_cast<int>(table) == number);
    CHECK(static_cast<int>(kLastTableName) == 24);
}
