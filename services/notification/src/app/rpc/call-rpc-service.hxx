#pragma once

#include <argus/notification/v1/notification.grpc.pb.h>
#include <feature/call/services/call-engine.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <vector>

class CallRpcService final
    : public argus::notification::v1::CallService::CallbackService
{
public:
  CallRpcService(std::shared_ptr<const CallEngine> engine,
                 NotificationCallCallers callers);

  grpc::ServerUnaryReactor*
  ClaimCall(grpc::CallbackServerContext* context,
            const argus::notification::v1::ClaimCallRequest* request,
            argus::notification::v1::ClaimCallResponse* response) override;

  grpc::ServerUnaryReactor*
  EndCall(grpc::CallbackServerContext* context,
          const argus::notification::v1::EndCallRequest* request,
          argus::notification::v1::EndCallResponse* response) override;

  grpc::ServerUnaryReactor*
  ScheduleCall(grpc::CallbackServerContext* context,
               const argus::notification::v1::ScheduleCallRequest* request,
               argus::notification::v1::ScheduleCallResponse* response) override;

  grpc::ServerUnaryReactor*
  AnnounceAgenda(grpc::CallbackServerContext* context,
                 const argus::notification::v1::AnnounceAgendaRequest* request,
                 argus::notification::v1::AnnounceAgendaResponse* response) override;

private:
  std::shared_ptr<const CallEngine> engine_;
  NotificationCallCallers callers_;
};
