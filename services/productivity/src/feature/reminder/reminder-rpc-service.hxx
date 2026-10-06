#pragma once

#include <argus/productivity/v1/reminder.grpc.pb.h>
#include <feature/reminder/services/reminder-feature-service.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>

#include <vector>

class ReminderRpcService final
    : public argus::productivity::v1::ReminderService::CallbackService
{
public:
  ReminderRpcService();
  explicit ReminderRpcService(std::vector<argus::client::CallerCredential> callers);

  grpc::ServerUnaryReactor*
  CreateReminder(grpc::CallbackServerContext* context,
                 const argus::productivity::v1::CreateReminderRequest* request,
                 argus::productivity::v1::ReminderResponse* response) override;

  grpc::ServerUnaryReactor*
  UpdateReminder(grpc::CallbackServerContext* context,
                 const argus::productivity::v1::UpdateReminderRequest* request,
                 argus::productivity::v1::ReminderResponse* response) override;

  grpc::ServerUnaryReactor* DeleteReminder(
      grpc::CallbackServerContext* context,
      const argus::productivity::v1::DeleteReminderRequest* request,
      argus::productivity::v1::DeleteReminderResponse* response) override;

  grpc::ServerUnaryReactor*
  GetReminder(grpc::CallbackServerContext* context,
              const argus::productivity::v1::GetReminderRequest* request,
              argus::productivity::v1::ReminderResponse* response) override;

  grpc::ServerUnaryReactor* ListReminders(
      grpc::CallbackServerContext* context,
      const argus::productivity::v1::ListRemindersRequest* request,
      argus::productivity::v1::ListRemindersResponse* response) override;

private:
  std::vector<argus::client::CallerCredential> callers_;
  ReminderFeatureService service_;
};
