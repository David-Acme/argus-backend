#include "health-rpc-service.hxx"

#include <atomic>

grpc::ServerUnaryReactor*
HealthRpcService::Check(grpc::CallbackServerContext* context,
                        const grpc::health::v1::HealthCheckRequest*,
                        grpc::health::v1::HealthCheckResponse* response)
{
  response->set_status(grpc::health::v1::HealthCheckResponse::SERVING);
  auto* reactor = context->DefaultReactor();
  reactor->Finish(grpc::Status::OK);
  return reactor;
}

namespace
{

class HealthWatchStream final
    : public grpc::ServerWriteReactor<grpc::health::v1::HealthCheckResponse>
{
public:
  explicit HealthWatchStream(grpc::health::v1::HealthCheckResponse initial)
      : message_(std::move(initial))
  {
    StartWrite(&message_);
  }

  void OnWriteDone(bool ok) override
  {
    if (!ok)
      finish(grpc::Status::CANCELLED);
  }

  void OnCancel() override { finish(grpc::Status::CANCELLED); }

  void OnDone() override { delete this; }

private:
  void finish(const grpc::Status& status)
  {
    if (!finished_.exchange(true))
      Finish(status);
  }

  grpc::health::v1::HealthCheckResponse message_;
  std::atomic<bool> finished_{false};
};

}

grpc::ServerWriteReactor<grpc::health::v1::HealthCheckResponse>*
HealthRpcService::Watch(grpc::CallbackServerContext*,
                        const grpc::health::v1::HealthCheckRequest*)
{
  grpc::health::v1::HealthCheckResponse response;
  response.set_status(grpc::health::v1::HealthCheckResponse::SERVING);
  return new HealthWatchStream(std::move(response));
}
