#include "grpc-tool-transport.hxx"

#include <grpc/grpc-client-base.hxx>
#include <mcp.grpc.pb.h>

#include <trantor/utils/Logger.h>

#include <stdexcept>
#include <utility>

namespace argus::mcp
{

namespace
{
constexpr std::chrono::milliseconds kLongestTimeout{120000};
}

struct GrpcToolTransport::Impl
{
  explicit Impl(ToolEndpoint input)
      : endpoint(std::move(input)),
        channel(argus::client::makeChannel(endpoint.target)),
        stub(v1::Mcp::NewStub(channel))
  {
  }

  ToolEndpoint endpoint;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<v1::Mcp::StubInterface> stub;
};

GrpcToolTransport::GrpcToolTransport(ToolEndpoint endpoint)
{
  if (endpoint.target.empty() || endpoint.credential.empty() || endpoint.timeout <= std::chrono::milliseconds::zero() ||
      endpoint.timeout > kLongestTimeout)
    throw std::invalid_argument("A tool endpoint needs a target, a credential and a timeout up to two minutes");
  impl_ = std::make_unique<Impl>(std::move(endpoint));
}

GrpcToolTransport::~GrpcToolTransport() = default;

std::optional<std::string> GrpcToolTransport::exchange(const std::string& frame)
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, static_cast<int>(impl_->endpoint.timeout.count()));
  argus::client::addCallerCredential(context, impl_->endpoint.credential);
  v1::Frame request;
  request.set_payload(frame);
  v1::Frame response;
  const grpc::Status status = impl_->stub->Rpc(&context, request, &response);
  if (!status.ok()) {
    LOG_WARN << "mcp: " << impl_->endpoint.target << " answered " << static_cast<int>(status.error_code()) << " "
             << status.error_message();
    return std::nullopt;
  }
  return response.payload();
}

}
