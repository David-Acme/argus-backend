#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/enums.hxx>
#include <cstddef>
#include <string>
#include <vector>

template <typename Enum>
static void checkRoundTrip(const std::vector<Enum>& values,
                           const std::vector<std::string>& names,
                           std::string (*toString)(Enum),
                           Enum (*fromString)(const std::string&))
{
    REQUIRE(values.size() == names.size());
    for (size_t i = 0; i < values.size(); ++i) {
        CHECK(toString(values[i]) == names[i]);
        CHECK(fromString(names[i]) == values[i]);
    }
}

TEST_CASE("user role strings round-trip")
{
    checkRoundTrip(
        {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest},
        {"owner", "resident", "guard", "guest"}, userRoleToString,
        userRoleFromString);
}

TEST_CASE("stored file category strings round-trip")
{
    checkRoundTrip({StoredFileCategory::Portrait, StoredFileCategory::Attachment},
                   {"portrait", "attachment"}, storedFileCategoryToString,
                   storedFileCategoryFromString);
}

TEST_CASE("portrait access request status strings round-trip")
{
    checkRoundTrip(
        {PortraitAccessRequestStatus::Pending, PortraitAccessRequestStatus::Approved,
         PortraitAccessRequestStatus::Denied, PortraitAccessRequestStatus::Cancelled},
        {"pending", "approved", "denied", "cancelled"},
        portraitAccessRequestStatusToString,
        portraitAccessRequestStatusFromString);
}

TEST_CASE("portrait access grant scope strings round-trip")
{
    checkRoundTrip({PortraitAccessGrantScope::Temporary,
                    PortraitAccessGrantScope::Permanent},
                   {"temporary", "permanent"}, portraitAccessGrantScopeToString,
                   portraitAccessGrantScopeFromString);
}

TEST_CASE("event severity strings round-trip")
{
    checkRoundTrip(
        {EventSeverity::Info, EventSeverity::Warning, EventSeverity::Critical},
        {"info", "warning", "critical"}, eventSeverityToString,
        eventSeverityFromString);
}

TEST_CASE("camera record mode strings round-trip")
{
    checkRoundTrip({CameraRecordMode::Events, CameraRecordMode::Continuous},
                   {"events", "continuous"}, cameraRecordModeToString,
                   cameraRecordModeFromString);
}

TEST_CASE("zone type strings round-trip")
{
    checkRoundTrip({ZoneType::Monitor, ZoneType::Alert, ZoneType::Exclude},
                   {"monitor", "alert", "exclude"}, zoneTypeToString,
                   zoneTypeFromString);
}

TEST_CASE("reminder detail status strings round-trip")
{
    checkRoundTrip(
        {ReminderDetailStatus::Pending, ReminderDetailStatus::InProgress,
         ReminderDetailStatus::Done, ReminderDetailStatus::Blocked},
        {"pending", "in_progress", "done", "blocked"},
        reminderDetailStatusToString, reminderDetailStatusFromString);
}

TEST_CASE("user action strings round-trip")
{
    checkRoundTrip(
        {UserAction::Create, UserAction::Read, UserAction::Update,
         UserAction::Delete},
        {"create", "read", "update", "delete"}, userActionToString,
        userActionFromString);
}

TEST_CASE("memory scope strings round-trip")
{
    checkRoundTrip(
        {MemoryScope::Global, MemoryScope::User, MemoryScope::Person,
         MemoryScope::Device, MemoryScope::Role},
        {"global", "user", "person", "device", "role"}, memoryScopeToString,
        memoryScopeFromString);
}

TEST_CASE("memory type strings round-trip")
{
    checkRoundTrip(
        {MemoryType::Persona, MemoryType::Episodic, MemoryType::Instruction,
         MemoryType::System},
        {"persona", "episodic", "instruction", "system"}, memoryTypeToString,
        memoryTypeFromString);
}

TEST_CASE("phrase kind strings round-trip")
{
    checkRoundTrip(
        {PhraseKind::Trigger, PhraseKind::Confirmation, PhraseKind::StatementStart,
         PhraseKind::RecallMarker, PhraseKind::Interrogative, PhraseKind::Filler,
         PhraseKind::Cancellation},
        {"trigger", "confirmation", "statement_start", "recall_marker",
         "interrogative", "filler", "cancellation"},
        phraseKindToString, phraseKindFromString);
}

TEST_CASE("lexicon kind strings round-trip")
{
    checkRoundTrip(
        {LexiconKind::Predicate, LexiconKind::Kinship, LexiconKind::FirstPerson,
         LexiconKind::Stopword},
        {"predicate", "kinship", "first_person", "stopword"},
        lexiconKindToString, lexiconKindFromString);
}

TEST_CASE("memory source strings round-trip")
{
    checkRoundTrip({MemorySource::Rule, MemorySource::Llm, MemorySource::Ingest},
                   {"rule", "llm", "ingest"}, memorySourceToString,
                   memorySourceFromString);
}

TEST_CASE("job state strings round-trip")
{
    checkRoundTrip(
        {JobState::Waiting, JobState::Active, JobState::Completed, JobState::Failed,
         JobState::Delayed},
        {"waiting", "active", "completed", "failed", "delayed"},
        jobStateToString, jobStateFromString);
}

TEST_CASE("camera driver strings round-trip")
{
    checkRoundTrip({CameraDriver::Tapo, CameraDriver::Onvif, CameraDriver::Rtsp},
                   {"tapo", "onvif", "rtsp"}, cameraDriverToString,
                   cameraDriverFromString);
}

TEST_CASE("share access strings round-trip")
{
    checkRoundTrip({ShareAccess::View, ShareAccess::Edit}, {"view", "edit"},
                   shareAccessToString, shareAccessFromString);
}

TEST_CASE("voice language strings round-trip")
{
    checkRoundTrip({VoiceLang::Es, VoiceLang::En}, {"es", "en"},
                   voiceLangToString, voiceLangFromString);
    CHECK(voiceLangToString(VoiceLang::System) == "");
    CHECK(voiceLangFromString("") == VoiceLang::System);
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
        TableName::Memory,
    };
    const std::vector<std::string> names = {
        "user",               "user_invitation",     "person",
        "event",              "reminder",            "reminder_detail",
        "calendar_event",     "calendar_event_share", "project",
        "project_member",     "project_task",        "context_note",
        "camera",             "camera_stream",       "zone",
        "audit_log",          "user_audit_log",      "notification",
        "notification_token", "user_action_log",     "refresh_token",
        "face_embedding",     "memory",
    };
    checkRoundTrip(values, names, tableNameToString, tableNameFromString);

    CHECK(tableNameToString(TableName::Memory) == "memory");
    CHECK(tableNameToString(TableName::PersonEvent) == "person_event");
}

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(userRoleFromString("bogus") == UserRole::Guest);
    CHECK(storedFileCategoryFromString("bogus") ==
          StoredFileCategory::Portrait);
    CHECK(portraitAccessRequestStatusFromString("bogus") ==
          PortraitAccessRequestStatus::Pending);
    CHECK(portraitAccessGrantScopeFromString("bogus") ==
          PortraitAccessGrantScope::Temporary);
    CHECK(eventSeverityFromString("bogus") == EventSeverity::Info);
    CHECK(cameraRecordModeFromString("bogus") == CameraRecordMode::Events);
    CHECK(zoneTypeFromString("bogus") == ZoneType::Monitor);
    CHECK(reminderDetailStatusFromString("bogus") ==
          ReminderDetailStatus::Pending);
    CHECK(userActionFromString("bogus") == UserAction::Create);
    CHECK(memoryScopeFromString("bogus") == MemoryScope::Global);
    CHECK(memoryTypeFromString("bogus") == MemoryType::Persona);
    CHECK(phraseKindFromString("bogus") == PhraseKind::Trigger);
    CHECK(lexiconKindFromString("bogus") == LexiconKind::Predicate);
    CHECK(memorySourceFromString("bogus") == MemorySource::Rule);
    CHECK(jobStateFromString("bogus") == JobState::Waiting);
    CHECK(tableNameFromString("bogus") == TableName::User);
    CHECK(cameraDriverFromString("bogus") == CameraDriver::Tapo);
    CHECK(shareAccessFromString("bogus") == ShareAccess::View);
    CHECK(voiceLangFromString("bogus") == VoiceLang::System);
}
