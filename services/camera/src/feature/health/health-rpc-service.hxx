#pragma once

#include <grpc/health/v1/health.grpc.pb.h>
#include <grpcpp/grpcpp.h>

class HealthRpcService final : public grpc::health::v1::Health::CallbackService
{
public:
  grpc::ServerUnaryReactor*
  Check(grpc::CallbackServerContext* context,
        const grpc::health::v1::HealthCheckRequest* request,
        grpc::health::v1::HealthCheckResponse* response) override;

  grpc::ServerWriteReactor<grpc::health::v1::HealthCheckResponse>*
  Watch(grpc::CallbackServerContext* context,
        const grpc::health::v1::HealthCheckRequest* request) override;
};
