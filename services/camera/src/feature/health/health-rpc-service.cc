#include "health-rpc-service.hxx"

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

// Streams the current status once and holds the stream open until cancel.
class HealthWatchStream final
    : public grpc::ServerWriteReactor<grpc::health::v1::HealthCheckResponse>
{
public:
  explicit HealthWatchStream(grpc::health::v1::HealthCheckResponse initial)
      : message_(std::move(initial))
  {
    StartWrite(&message_);
  }

  void OnWriteDone(bool ok) override { (void)ok; }

  void OnDone() override {}

private:
  grpc::health::v1::HealthCheckResponse message_;
};

} // namespace

grpc::ServerWriteReactor<grpc::health::v1::HealthCheckResponse>*
HealthRpcService::Watch(grpc::CallbackServerContext*,
                        const grpc::health::v1::HealthCheckRequest*)
{
  grpc::health::v1::HealthCheckResponse response;
  response.set_status(grpc::health::v1::HealthCheckResponse::SERVING);
  return new HealthWatchStream(std::move(response));
}
