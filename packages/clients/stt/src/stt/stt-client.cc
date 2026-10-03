#include "stt-client.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <response/response-rpc.hxx>
#include <stt.grpc.pb.h>
#include <stt/stt-errors.hxx>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
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

using Clock = std::chrono::system_clock;
using StreamIo = grpc::ClientReaderWriter<v1::TranscribeChunk, v1::TranscribeUpdate>;

StreamUpdate fromWire(const v1::TranscribeUpdate& update)
{
  return {.text = update.text(),
          .final = update.final(),
          .samples = static_cast<std::size_t>(update.samples()),
          .decodeMs = static_cast<int>(update.decode_ms())};
}
}

struct TranscribeStream::Impl
{
  grpc::ClientContext context;
  StreamInput input;
  Clock::time_point deadlineAt;
  std::unique_ptr<StreamIo> io;
  std::optional<std::stop_callback<std::function<void()>>> onStop;
  std::mutex mutex;
  std::optional<StreamUpdate> last;
  std::jthread reader;
  bool started{false};
  bool closed{false};
  bool ended{false};

  void read()
  {
    v1::TranscribeUpdate update;
    while (io->Read(&update)) {
      const StreamUpdate received = fromWire(update);
      if (received.final) {
        std::scoped_lock lock(mutex);
        last = received;
        continue;
      }
      if (input.onPartial)
        input.onPartial(received);
    }
  }

  [[noreturn]] void fail()
  {
    if (reader.joinable())
      reader.join();
    ended = true;
    const grpc::Status status = io->Finish();
    if (input.cancellation.stop_requested())
      throw ResponseException(499, SttErrors::Cancelled);
    if (status.error_code() == grpc::StatusCode::CANCELLED && Clock::now() >= deadlineAt)
      throw ResponseException(504, SttErrors::DeadlineExceeded);
    if (!status.ok())
      throw argus::response::fromRpcStatus(status);
    throw ResponseException(502, SttErrors::InvalidResponse);
  }

  void write(std::span<const float> samples, bool flush)
  {
    if (closed || ended)
      throw ResponseException(400, SttErrors::InvalidRequest);
    v1::TranscribeChunk chunk;
    if (!started) {
      chunk.set_sample_rate(static_cast<std::uint32_t>(input.sampleRate));
      chunk.set_language(input.language);
      started = true;
    }
    chunk.mutable_samples()->Add(samples.begin(), samples.end());
    chunk.set_flush(flush);
    if (!io->Write(chunk))
      fail();
  }
};

TranscribeStream::TranscribeStream(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

TranscribeStream::~TranscribeStream()
{
  if (impl_ && impl_->reader.joinable()) {
    impl_->context.TryCancel();
    impl_->reader.join();
  }
}

void TranscribeStream::push(std::span<const float> samples)
{
  if (!samples.empty())
    impl_->write(samples, false);
}

void TranscribeStream::flush()
{
  impl_->write({}, true);
}

void TranscribeStream::cancel()
{
  impl_->context.TryCancel();
}

StreamUpdate TranscribeStream::finish()
{
  if (impl_->ended)
    throw ResponseException(400, SttErrors::InvalidRequest);
  if (!impl_->closed) {
    impl_->closed = true;
    impl_->io->WritesDone();
  }
  if (impl_->reader.joinable())
    impl_->reader.join();
  std::optional<StreamUpdate> last;
  {
    std::scoped_lock lock(impl_->mutex);
    last = impl_->last;
  }
  if (!last)
    impl_->fail();
  impl_->ended = true;
  const grpc::Status status = impl_->io->Finish();
  if (!status.ok())
    throw argus::response::fromRpcStatus(status);
  return *last;
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

std::unique_ptr<TranscribeStream> Client::openStream(StreamInput input) const
{
  if (!validRate(input.sampleRate))
    throw ResponseException(400, SttErrors::InvalidRequest);
  if (input.cancellation.stop_requested())
    throw ResponseException(499, SttErrors::Cancelled);
  auto impl = std::make_unique<TranscribeStream::Impl>();
  impl->input = std::move(input);
  impl->deadlineAt = Clock::now() + impl_->config.timeout;
  impl->context.set_deadline(impl->deadlineAt);
  argus::client::addCallerCredential(impl->context, impl_->config.credential);
  impl->io = impl_->stub->TranscribeStream(&impl->context);
  auto* raw = impl.get();
  impl->onStop.emplace(raw->input.cancellation, [raw] { raw->context.TryCancel(); });
  impl->reader = std::jthread([raw] { raw->read(); });
  return std::make_unique<TranscribeStream>(std::move(impl));
}
}
