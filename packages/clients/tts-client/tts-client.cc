#include "tts-client.hxx"

#include <grpc-client-base.hxx>
#include <tts.grpc.pb.h>
#include <response-rpc.hxx>
#include <tts-errors.hxx>
#include <cmath>
#include <utility>

namespace argus::tts
{
namespace
{
void check(const grpc::Status& status)
{
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
}

bool validRate(int rate)
{
  return rate > 0 && rate <= 192000;
}
}

struct Client::Impl
{
  ClientConfig config;
  std::unique_ptr<v1::Synthesis::Stub> stub;
};

Client::Client(ClientConfig config)
{
  if (config.target.empty() || config.credential.empty() ||
      config.timeout.count() <= 0 || config.timeout > std::chrono::seconds(120))
    throw ResponseException(400, TtsErrors::InvalidRequest);
  auto stub = v1::Synthesis::NewStub(argus::client::makeChannel(config.target));
  impl_ = std::make_unique<Impl>(Impl{.config = std::move(config),
                                    .stub = std::move(stub)});
}

Client::~Client() = default;

Capabilities Client::capabilities(std::stop_token cancellation) const
{
  if (cancellation.stop_requested())
    throw ResponseException(499, TtsErrors::Cancelled);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + impl_->config.timeout);
  argus::client::addCallerCredential(context, impl_->config.credential);
  v1::CapabilitiesRequest request;
  v1::CapabilitiesResponse response;
  std::stop_callback cancel(cancellation, [&context] { context.TryCancel(); });
  const auto status = impl_->stub->Capabilities(&context, request, &response);
  if (cancellation.stop_requested())
    throw ResponseException(499, TtsErrors::Cancelled);
  check(status);
  if (!validRate(response.sample_rate()) || response.channels() != 1 ||
      response.format() != v1::SAMPLE_FORMAT_FLOAT32 ||
      !std::isfinite(response.default_speed()) || response.default_speed() <= 0)
    throw ResponseException(502, TtsErrors::InvalidResponse);
  return {.sampleRate = static_cast<int>(response.sample_rate()),
          .channels = static_cast<int>(response.channels()),
          .defaultSpeed = response.default_speed(),
          .voices = {response.voices().begin(), response.voices().end()},
          .languages = {response.languages().begin(), response.languages().end()}};
}

void Client::synthesize(const SynthesisInput& input) const
{
  if (!input.onChunk)
    throw ResponseException(400, TtsErrors::InvalidRequest);
  if (input.cancellation.stop_requested())
    throw ResponseException(499, TtsErrors::Cancelled);
  grpc::ClientContext context;
  const auto deadlineAt = std::chrono::system_clock::now() + impl_->config.timeout;
  context.set_deadline(deadlineAt);
  argus::client::addCallerCredential(context, impl_->config.credential);
  std::stop_callback cancel(input.cancellation, [&context] { context.TryCancel(); });
  v1::SynthesisRequest request;
  request.set_text(input.text);
  request.set_voice(input.voice);
  request.set_language(input.language);
  request.set_speed(input.speed);
  request.set_quality(static_cast<v1::Quality>(input.quality));
  auto reader = impl_->stub->Synthesize(&context, request);
  v1::AudioChunk response;
  std::uint64_t sequence = 0;
  int rate = 0;
  bool stopped = false;
  try {
    while (reader->Read(&response)) {
      if (input.cancellation.stop_requested()) {
        context.TryCancel();
        break;
      }
      if (!validRate(response.sample_rate()) || response.channels() != 1 ||
          response.format() != v1::SAMPLE_FORMAT_FLOAT32 ||
          response.sequence() != sequence++ || response.samples().empty() ||
          response.samples_size() > 4096 ||
          (rate != 0 && rate != static_cast<int>(response.sample_rate())))
        throw ResponseException(502, TtsErrors::InvalidResponse);
      rate = static_cast<int>(response.sample_rate());
      AudioChunk chunk{.sampleRate = rate,
                       .sequence = response.sequence(),
                       .samples = {response.samples().begin(), response.samples().end()}};
      if (!input.onChunk(std::move(chunk))) {
        stopped = true;
        context.TryCancel();
        break;
      }
    }
  }
  catch (...) {
    context.TryCancel();
    reader->Finish();
    throw;
  }
  const auto status = reader->Finish();
  const auto deadlinePassed = std::chrono::system_clock::now() >= deadlineAt;
  if (stopped || input.cancellation.stop_requested())
    throw ResponseException(499, TtsErrors::Cancelled);
  if (status.error_code() == grpc::StatusCode::CANCELLED && deadlinePassed)
    throw ResponseException(504, TtsErrors::DeadlineExceeded);
  check(status);
  if (sequence == 0)
    throw ResponseException(502, TtsErrors::InvalidResponse);
}
}
