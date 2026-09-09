#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/enums.hxx>
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

TEST_CASE("user role strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {UserRole::Owner, UserRole::Resident, UserRole::Guard,
                   UserRole::Guest},
        .names = {"owner", "resident", "guard", "guest"},
        .toString = userRoleToString,
        .fromString = userRoleFromString});
}

TEST_CASE("stored file category strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {StoredFileCategory::Portrait, StoredFileCategory::Attachment},
        .names = {"portrait", "attachment"},
        .toString = storedFileCategoryToString,
        .fromString = storedFileCategoryFromString});
}

TEST_CASE("event severity strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {EventSeverity::Info, EventSeverity::Warning,
                   EventSeverity::Critical},
        .names = {"info", "warning", "critical"},
        .toString = eventSeverityToString,
        .fromString = eventSeverityFromString});
}

TEST_CASE("camera record mode strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {CameraRecordMode::Events, CameraRecordMode::Continuous},
        .names = {"events", "continuous"},
        .toString = cameraRecordModeToString,
        .fromString = cameraRecordModeFromString});
}

TEST_CASE("zone type strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {ZoneType::Monitor, ZoneType::Alert, ZoneType::Exclude},
        .names = {"monitor", "alert", "exclude"},
        .toString = zoneTypeToString,
        .fromString = zoneTypeFromString});
}

TEST_CASE("reminder detail status strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {ReminderDetailStatus::Pending, ReminderDetailStatus::InProgress,
                   ReminderDetailStatus::Done, ReminderDetailStatus::Blocked},
        .names = {"pending", "in_progress", "done", "blocked"},
        .toString = reminderDetailStatusToString,
        .fromString = reminderDetailStatusFromString});
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

TEST_CASE("memory scope strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {MemoryScope::Global, MemoryScope::User, MemoryScope::Person,
                   MemoryScope::Device, MemoryScope::Role},
        .names = {"global", "user", "person", "device", "role"},
        .toString = memoryScopeToString,
        .fromString = memoryScopeFromString});
}

TEST_CASE("memory type strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {MemoryType::Persona, MemoryType::Episodic, MemoryType::Instruction,
                   MemoryType::System},
        .names = {"persona", "episodic", "instruction", "system"},
        .toString = memoryTypeToString,
        .fromString = memoryTypeFromString});
}

TEST_CASE("phrase kind strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {PhraseKind::Trigger, PhraseKind::Confirmation,
                   PhraseKind::StatementStart, PhraseKind::RecallMarker,
                   PhraseKind::Interrogative, PhraseKind::Filler,
                   PhraseKind::Cancellation},
        .names = {"trigger", "confirmation", "statement_start", "recall_marker",
                  "interrogative", "filler", "cancellation"},
        .toString = phraseKindToString,
        .fromString = phraseKindFromString});
}

TEST_CASE("lexicon kind strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {LexiconKind::Predicate, LexiconKind::Kinship,
                   LexiconKind::FirstPerson, LexiconKind::Stopword},
        .names = {"predicate", "kinship", "first_person", "stopword"},
        .toString = lexiconKindToString,
        .fromString = lexiconKindFromString});
}

TEST_CASE("memory source strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {MemorySource::Rule, MemorySource::Llm, MemorySource::Ingest},
        .names = {"rule", "llm", "ingest"},
        .toString = memorySourceToString,
        .fromString = memorySourceFromString});
}

TEST_CASE("camera driver strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {CameraDriver::Tapo, CameraDriver::Onvif, CameraDriver::Rtsp},
        .names = {"tapo", "onvif", "rtsp"},
        .toString = cameraDriverToString,
        .fromString = cameraDriverFromString});
}

TEST_CASE("share access strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {ShareAccess::View, ShareAccess::Edit},
        .names = {"view", "edit"},
        .toString = shareAccessToString,
        .fromString = shareAccessFromString});
}

TEST_CASE("voice language strings round-trip")
{
    checkRoundTrip(CheckRoundTripInput{
        .values = {VoiceLang::Es, VoiceLang::En},
        .names = {"es", "en"},
        .toString = voiceLangToString,
        .fromString = voiceLangFromString});
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
    checkRoundTrip(CheckRoundTripInput{.values = values,
                                        .names = names,
                                        .toString = tableNameToString,
                                        .fromString = tableNameFromString});

    CHECK(tableNameToString(TableName::Memory) == "memory");
    CHECK(tableNameToString(TableName::PersonEvent) == "person_event");
}

TEST_CASE("unknown strings fall back to documented defaults")
{
    CHECK(userRoleFromString("bogus") == UserRole::Guest);
    CHECK(storedFileCategoryFromString("bogus") ==
          StoredFileCategory::Portrait);
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
    CHECK(tableNameFromString("bogus") == TableName::User);
    CHECK(cameraDriverFromString("bogus") == CameraDriver::Tapo);
    CHECK(shareAccessFromString("bogus") == ShareAccess::View);
    CHECK(voiceLangFromString("bogus") == VoiceLang::System);
}
