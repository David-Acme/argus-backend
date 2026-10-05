#include "stt-rpc-server.hxx"

#include <errors/response-exception.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <response/response-rpc.hxx>
#include <stt.grpc.pb.h>
#include <stt/stt-errors.hxx>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <semaphore>
#include <stdexcept>

namespace
{
namespace wire = argus::stt::v1;
using Clock = std::chrono::system_clock;

constexpr int kMinSampleRate = 8000;
constexpr int kMaxSampleRate = 192000;
constexpr auto kMaxDeadline = std::chrono::seconds(120);
constexpr int kMaxReceiveBytes = 64 * 1024 * 1024;
constexpr std::size_t kMaxStreamSeconds = 120;
constexpr std::chrono::milliseconds kSlotPoll{20};

using StreamIo = grpc::ServerReaderWriter<wire::TranscribeUpdate, wire::TranscribeChunk>;

ResponseException stoppedError(const grpc::ServerContext& context)
{
  if (Clock::now() >= context.deadline())
    return {504, SttErrors::DeadlineExceeded};
  return {499, SttErrors::Cancelled};
}

bool validRate(std::uint32_t rate)
{
  return rate >= static_cast<std::uint32_t>(kMinSampleRate) &&
         rate <= static_cast<std::uint32_t>(kMaxSampleRate);
}

bool deadlineTooFar(const grpc::ServerContext& context)
{
  return context.deadline() != Clock::time_point::max() &&
         context.deadline() > Clock::now() + kMaxDeadline;
}

struct StreamAudio
{
  TranscribeRequest request;
  std::size_t decoded{0};
  std::string text;
  bool started{false};

  [[nodiscard]] bool pending() const { return request.samples.size() > decoded; }

  [[nodiscard]] bool accept(const wire::TranscribeChunk& chunk,
                            const std::function<bool(const std::string&)>& acceptsLanguage)
  {
    if (!started) {
      if (!validRate(chunk.sample_rate()) ||
          (!chunk.language().empty() && !acceptsLanguage(chunk.language())))
        return false;
      request.sampleRate = static_cast<int32_t>(chunk.sample_rate());
      request.lang = chunk.language();
      started = true;
    }
    else if ((chunk.sample_rate() != 0 &&
              chunk.sample_rate() != static_cast<std::uint32_t>(request.sampleRate)) ||
             (!chunk.language().empty() && chunk.language() != request.lang)) {
      return false;
    }
    const std::size_t limit = static_cast<std::size_t>(request.sampleRate) * kMaxStreamSeconds;
    if (request.samples.size() + static_cast<std::size_t>(chunk.samples_size()) > limit)
      return false;
    request.samples.insert(request.samples.end(), chunk.samples().begin(), chunk.samples().end());
    return true;
  }

  [[nodiscard]] wire::TranscribeUpdate update(bool final, std::uint32_t decodeMs) const
  {
    wire::TranscribeUpdate out;
    out.set_text(text);
    out.set_final(final);
    out.set_samples(decoded);
    out.set_decode_ms(decodeMs);
    return out;
  }
};

bool validRequest(const wire::TranscribeRequest& request,
                  const std::function<bool(const std::string&)>& acceptsLanguage)
{
  return !request.samples().empty() && validRate(request.sample_rate()) &&
         (request.language().empty() || acceptsLanguage(request.language()));
}
}

struct SttRpcServer::Impl final : wire::Transcription::Service
{
  explicit Impl(SttRpcInput input)
      : input_(std::move(input)),
        gate_({.expectedCallers = {},
               .callerPairs = input_.credentials,
               .legacySecret = {},
               .onFirstLegacy = {}}),
        slots_(std::max(1, input_.slots))
  {
    if (!input_.transcribe || !input_.capabilities || !input_.acceptsLanguage ||
        gate_.pairedCount() == 0)
      throw std::invalid_argument("Invalid STT RPC configuration");
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(kMaxReceiveBytes);
    builder.AddListeningPort(input_.address, grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(this);
    for (auto* service : input_.services)
      builder.RegisterService(service);
    server_ = builder.BuildAndStart();
    if (!server_)
      throw std::runtime_error("STT RPC listener failed");
  }

  grpc::Status Capabilities(grpc::ServerContext* context,
                            const wire::CapabilitiesRequest*,
                            wire::CapabilitiesResponse* response) override
  {
    if (!gate_.admit(context, {}).admitted())
      return argus::response::toRpcStatus(ResponseException(401, SttErrors::Unauthorized));
    argus::stt::Capabilities capabilities;
    try {
      capabilities = input_.capabilities();
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(ResponseException(500, SttErrors::InternalError));
    }
    response->set_sample_rate(static_cast<std::uint32_t>(capabilities.sampleRate));
    response->set_loaded(capabilities.loaded);
    response->set_language(capabilities.language);
    response->set_default_language(capabilities.defaultLanguage);
    for (const auto& language : capabilities.languages)
      response->add_languages(language);
    return grpc::Status::OK;
  }

  grpc::Status Transcribe(grpc::ServerContext* context,
                          const wire::TranscribeRequest* request,
                          wire::TranscribeResponse* response) override
  {
    if (!gate_.admit(context, {}).admitted())
      return argus::response::toRpcStatus(ResponseException(401, SttErrors::Unauthorized));
    try {
      if (!validRequest(*request, input_.acceptsLanguage))
        return argus::response::toRpcStatus(ResponseException(400, SttErrors::InvalidRequest));
      if (deadlineTooFar(*context))
        return argus::response::toRpcStatus(ResponseException(400, SttErrors::InvalidRequest));
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(ResponseException(500, SttErrors::InternalError));
    }
    if (context->IsCancelled() || Clock::now() >= context->deadline())
      return argus::response::toRpcStatus(stoppedError(*context));
    if (!slots_.try_acquire())
      return argus::response::toRpcStatus(ResponseException(429, SttErrors::Busy));
    struct Release
    {
      std::counting_semaphore<>& slots;
      ~Release() { slots.release(); }
    } release{slots_};
    try {
      response->set_text(input_.transcribe(
          {.samples = {request->samples().begin(), request->samples().end()},
           .sampleRate = static_cast<int32_t>(request->sample_rate()),
           .lang = request->language()}));
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(ResponseException(500, SttErrors::InternalError));
    }
    return grpc::Status::OK;
  }

  grpc::Status TranscribeStream(grpc::ServerContext* context, StreamIo* stream) override
  {
    if (!gate_.admit(context, {}).admitted())
      return argus::response::toRpcStatus(ResponseException(401, SttErrors::Unauthorized));
    if (deadlineTooFar(*context))
      return argus::response::toRpcStatus(ResponseException(400, SttErrors::InvalidRequest));
    try {
      StreamAudio audio;
      wire::TranscribeChunk chunk;
      while (stream->Read(&chunk)) {
        if (!audio.accept(chunk, input_.acceptsLanguage))
          return argus::response::toRpcStatus(ResponseException(400, SttErrors::InvalidRequest));
        if (chunk.flush() && !audio.request.samples.empty() &&
            !stream->Write(partial(audio)))
          return argus::response::toRpcStatus(stoppedError(*context));
      }
      if (context->IsCancelled())
        return argus::response::toRpcStatus(stoppedError(*context));
      if (audio.request.samples.empty())
        return argus::response::toRpcStatus(ResponseException(400, SttErrors::InvalidRequest));
      if (!stream->Write(finalUpdate(*context, audio)))
        return argus::response::toRpcStatus(stoppedError(*context));
    }
    catch (const ResponseException& error) {
      return argus::response::toRpcStatus(error);
    }
    catch (...) {
      return argus::response::toRpcStatus(ResponseException(500, SttErrors::InternalError));
    }
    return grpc::Status::OK;
  }

  std::uint32_t decode(StreamAudio& audio)
  {
    struct Release
    {
      std::counting_semaphore<>& slots;
      ~Release() { slots.release(); }
    } release{slots_};
    const auto started = std::chrono::steady_clock::now();
    audio.text = input_.transcribe(audio.request);
    audio.decoded = audio.request.samples.size();
    return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count());
  }

  wire::TranscribeUpdate partial(StreamAudio& audio)
  {
    if (!audio.pending() || !slots_.try_acquire())
      return audio.update(false, 0);
    const std::uint32_t ms = decode(audio);
    return audio.update(false, ms);
  }

  wire::TranscribeUpdate finalUpdate(grpc::ServerContext& context, StreamAudio& audio)
  {
    if (!audio.pending())
      return audio.update(true, 0);
    const auto limit = std::min(context.deadline(), Clock::now() + kMaxDeadline);
    while (!slots_.try_acquire_for(kSlotPoll)) {
      if (context.IsCancelled() || Clock::now() >= context.deadline())
        throw stoppedError(context);
      if (Clock::now() >= limit)
        throw ResponseException(429, SttErrors::Busy);
    }
    const std::uint32_t ms = decode(audio);
    return audio.update(true, ms);
  }

  SttRpcInput input_;
  argus::client::FleetCallerGate gate_;
  std::counting_semaphore<> slots_;
  int port_{0};
  std::unique_ptr<grpc::Server> server_;
};

SttRpcServer::SttRpcServer(SttRpcInput input)
    : impl_(std::make_unique<Impl>(std::move(input)))
{
}

SttRpcServer::~SttRpcServer() { shutdown(); }
int SttRpcServer::port() const { return impl_->port_; }
void SttRpcServer::shutdown()
{
  if (impl_->server_) {
    impl_->server_->Shutdown(Clock::now());
    impl_->server_->Wait();
    impl_->server_.reset();
  }
}
