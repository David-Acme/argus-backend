#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <shared/repositories/calendar-event-share/calendar-event-share-repository.hxx>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <string>
#include <sync/table-name.hxx>
#include <sync/user-action.hxx>

struct ActionJournalInput
{
  int64_t actorId{0};
  TableName table{TableName::Reminder};
  int64_t recordId{0};
  UserAction action{UserAction::Create};
  std::string title;
  int64_t sharingParentId{0};
  drogon::orm::DbClient* client{nullptr};
};

class ActionJournal
{
public:
  [[nodiscard]] drogon::Task<void> record(const ActionJournalInput& input) const;

private:
  [[nodiscard]] drogon::Task<bool> shared(const ActionJournalInput& input) const;

  ProjectMemberRepository memberRepository_;
  CalendarEventShareRepository shareRepository_;
};
