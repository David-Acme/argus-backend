#include "stt-client.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <response/response-rpc.hxx>
#include <stt.grpc.pb.h>
#include <stt/stt-errors.hxx>
#include <utility>

namespace argus::stt
{
namespace
{
void check(const grpc::Status& status)
{
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
}

constexpr std::int64_t kMinSampleRate{8000};
constexpr std::int64_t kMaxSampleRate{192000};

bool validRate(std::int64_t rate)
{
  return rate >= kMinSampleRate && rate <= kMaxSampleRate;
}
}

struct Client::Impl
{
  ClientConfig config;
  std::unique_ptr<v1::Transcription::Stub> stub;
};

Client::Client(ClientConfig config)
{
  if (config.target.empty() || config.credential.empty())
    throw ResponseException(400, SttErrors::InvalidRequest);
  if (config.timeout.count() <= 0 || config.timeout > kMaxTimeout)
    throw ResponseException(
        400, SttErrors::InvalidRequest.withMessage(
                 "timeout must be within 1 and 120000 ms"));
  auto stub = v1::Transcription::NewStub(argus::client::makeChannel(config.target));
  impl_ = std::make_unique<Impl>(Impl{.config = std::move(config),
                                    .stub = std::move(stub)});
}

Client::~Client() = default;

Capabilities Client::capabilities(const std::stop_token& cancellation) const
{
  if (cancellation.stop_requested())
    throw ResponseException(499, SttErrors::Cancelled);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + impl_->config.timeout);
  argus::client::addCallerCredential(context, impl_->config.credential);
  v1::CapabilitiesRequest request;
  v1::CapabilitiesResponse response;
  std::stop_callback cancel(cancellation, [&context] { context.TryCancel(); });
  const auto status = impl_->stub->Capabilities(&context, request, &response);
  if (cancellation.stop_requested())
    throw ResponseException(499, SttErrors::Cancelled);
  check(status);
  if (!validRate(response.sample_rate()) || response.language().empty() ||
      response.default_language().empty() || response.languages().empty())
    throw ResponseException(502, SttErrors::InvalidResponse);
  return {.sampleRate = static_cast<int>(response.sample_rate()),
          .loaded = response.loaded(),
          .language = response.language(),
          .defaultLanguage = response.default_language(),
          .languages = {response.languages().begin(), response.languages().end()}};
}

std::string Client::transcribe(const TranscribeInput& input) const
{
  if (input.samples.empty() || !validRate(input.sampleRate))
    throw ResponseException(400, SttErrors::InvalidRequest);
  if (input.cancellation.stop_requested())
    throw ResponseException(499, SttErrors::Cancelled);
  grpc::ClientContext context;
  const auto deadlineAt = std::chrono::system_clock::now() + impl_->config.timeout;
  context.set_deadline(deadlineAt);
  argus::client::addCallerCredential(context, impl_->config.credential);
  std::stop_callback cancel(input.cancellation, [&context] { context.TryCancel(); });
  v1::TranscribeRequest request;
  request.mutable_samples()->Add(input.samples.begin(), input.samples.end());
  request.set_sample_rate(static_cast<std::uint32_t>(input.sampleRate));
  request.set_language(input.language);
  v1::TranscribeResponse response;
  const auto status = impl_->stub->Transcribe(&context, request, &response);
  if (input.cancellation.stop_requested())
    throw ResponseException(499, SttErrors::Cancelled);
  if (status.error_code() == grpc::StatusCode::CANCELLED &&
      std::chrono::system_clock::now() >= deadlineAt)
    throw ResponseException(504, SttErrors::DeadlineExceeded);
  check(status);
  return response.text();
}
}
