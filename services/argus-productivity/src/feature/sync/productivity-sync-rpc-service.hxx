#pragma once

#include <argus/productivity/v1/sync.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <shared/repositories/calendar-event-share/calendar-event-share-repository.hxx>
#include <shared/repositories/calendar-event/calendar-event-repository.hxx>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project-task/project-task-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <shared/repositories/reminder-detail/reminder-detail-repository.hxx>
#include <shared/repositories/reminder/reminder-repository.hxx>

// argus.productivity.v1.SyncService: productivity-domain sync-table pulls.
class ProductivitySyncRpcService final
    : public argus::productivity::v1::SyncService::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::productivity::v1::PullTableRequest* request,
            argus::productivity::v1::PullTableResponse* response) override;

private:
  ReminderRepository reminderRepository_;
  ReminderDetailRepository reminderDetailRepository_;
  CalendarEventRepository calendarEventRepository_;
  CalendarEventShareRepository calendarEventShareRepository_;
  ProjectRepository projectRepository_;
  ProjectMemberRepository projectMemberRepository_;
  ProjectTaskRepository projectTaskRepository_;
};
