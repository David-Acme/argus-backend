#include "vlm-client.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <response/response-rpc.hxx>
#include <utility>
#include <vlm.grpc.pb.h>
#include <vlm/vlm-errors.hxx>

namespace argus::vlm
{
namespace
{
void check(const grpc::Status& status)
{
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
}

constexpr std::int64_t kMaxInputPx{16384};
constexpr std::int64_t kMaxTokens{4096};

bool validCapabilities(const v1::CapabilitiesResponse& response)
{
  return response.max_input_px() > 0 &&
         response.max_input_px() <= kMaxInputPx &&
         response.default_max_tokens() >= 0 &&
         response.default_max_tokens() <= kMaxTokens;
}
}

struct Client::Impl
{
  ClientConfig config;
  std::unique_ptr<v1::Vision::Stub> stub;
};

Client::Client(ClientConfig config)
{
  if (config.target.empty() || config.credential.empty())
    throw ResponseException(400, VlmErrors::InvalidRequest);
  if (config.timeout.count() <= 0 || config.timeout > kMaxTimeout)
    throw ResponseException(
        400, VlmErrors::InvalidRequest.withMessage(
                 "timeout must be within 1 and 120000 ms"));
  auto stub = v1::Vision::NewStub(argus::client::makeChannel(config.target));
  impl_ = std::make_unique<Impl>(Impl{.config = std::move(config),
                                      .stub = std::move(stub)});
}

Client::~Client() = default;

Capabilities Client::capabilities() const
{
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + impl_->config.timeout);
  argus::client::addCallerCredential(context, impl_->config.credential);
  v1::CapabilitiesRequest request;
  v1::CapabilitiesResponse response;
  check(impl_->stub->Capabilities(&context, request, &response));
  if (!validCapabilities(response))
    throw ResponseException(502, VlmErrors::InvalidResponse);
  return {.loaded = response.loaded(),
          .maxInputPx = response.max_input_px(),
          .defaultMaxTokens = response.default_max_tokens()};
}

std::string Client::describe(const DescribeInput& input) const
{
  if (input.jpeg.empty() || input.maxTokens < 0)
    throw ResponseException(400, VlmErrors::InvalidRequest);
  grpc::ClientContext context;
  const auto deadlineAt = std::chrono::system_clock::now() + impl_->config.timeout;
  context.set_deadline(deadlineAt);
  argus::client::addCallerCredential(context, impl_->config.credential);
  v1::DescribeRequest request;
  request.set_image_jpeg(input.jpeg);
  request.set_prompt(input.prompt);
  request.set_camera_id(input.cameraId);
  request.set_max_tokens(input.maxTokens);
  v1::DescribeResponse response;
  const auto status = impl_->stub->Describe(&context, request, &response);
  if (status.error_code() == grpc::StatusCode::CANCELLED &&
      std::chrono::system_clock::now() >= deadlineAt)
    throw ResponseException(504, VlmErrors::DeadlineExceeded);
  check(status);
  if (response.caption().empty())
    throw ResponseException(502, VlmErrors::InvalidResponse);
  return response.caption();
}
}
