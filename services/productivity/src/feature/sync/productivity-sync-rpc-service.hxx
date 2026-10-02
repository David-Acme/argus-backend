#pragma once

#include <argus/productivity/v1/sync.grpc.pb.h>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <shared/repositories/calendar-event-share/calendar-event-share-repository.hxx>
#include <shared/repositories/calendar-event/calendar-event-repository.hxx>
#include <shared/repositories/project-member/project-member-repository.hxx>
#include <shared/repositories/project-task/project-task-repository.hxx>
#include <shared/repositories/project/project-repository.hxx>
#include <feature/sync/repositories/reminder-detail/reminder-detail-repository.hxx>
#include <feature/sync/repositories/reminder/reminder-repository.hxx>
#include <vector>

class ProductivitySyncRpcService final
    : public argus::productivity::v1::SyncService::CallbackService
{
public:
  ProductivitySyncRpcService();

  grpc::ServerUnaryReactor*
  PullTable(grpc::CallbackServerContext* context,
            const argus::productivity::v1::PullTableRequest* request,
            argus::productivity::v1::PullTableResponse* response) override;

private:
  std::vector<argus::client::CallerCredential> syncCallers_;
  ReminderRepository reminderRepository_;
  ReminderDetailRepository reminderDetailRepository_;
  CalendarEventRepository calendarEventRepository_;
  CalendarEventShareRepository calendarEventShareRepository_;
  ProjectRepository projectRepository_;
  ProjectMemberRepository projectMemberRepository_;
  ProjectTaskRepository projectTaskRepository_;
};
