#include "tts-rpc-server.hxx"

#include <errors/response-exception.hxx>
#include <grpc-server-identity.hxx>
#include <tts-errors.hxx>
#include <tts.grpc.pb.h>
#include <response-rpc.hxx>
#include <shared/wrapper/cancellation/cancellation-token.hxx>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <semaphore>
#include <thread>

namespace
{
namespace wire = argus::tts::v1;
using Clock = std::chrono::system_clock;

struct StreamQueue
{
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::vector<float>> chunks;
  bool done{false};
  bool stopped{false};
  grpc::Status status;
};

ResponseException stoppedError(const grpc::ServerContext& context)
{
  if (Clock::now() >= context.deadline())
    return ResponseException(504, TtsErrors::DeadlineExceeded);
  return ResponseException(499, TtsErrors::Cancelled);
}

bool authorized(grpc::ServerContext& context,
                const std::vector<std::pair<std::string, std::string>>& credentials)
{
  const auto& metadata = context.client_metadata();
  const auto range = metadata.equal_range(argus::client::kCallerCredentialKey);
  if (range.first == range.second || std::next(range.first) != range.second)
    return false;
  const std::string presented(range.first->second.data(), range.first->second.size());
  return std::ranges::any_of(credentials, [&presented](const auto& credential) {
    return !credential.first.empty() && !credential.second.empty() &&
           argus::client::constantTimeEquals(presented, credential.second);
  });
}

bool validRequest(const wire::SynthesisRequest& request,
                  const argus::tts::Capabilities& capabilities)
{
  return !request.text().empty() && request.text().size() <= 16384 &&
         request.text().find('\0') == std::string::npos &&
         std::ranges::find(capabilities.voices, request.voice()) != capabilities.voices.end() &&
         std::ranges::find(capabilities.languages, request.language()) != capabilities.languages.end() &&
         std::isfinite(request.speed()) &&
         (request.speed() == 0 || (request.speed() >= 0.7F && request.speed() <= 2.0F)) &&
         wire::Quality_IsValid(request.quality());
}
}

struct TtsRpcServer::Impl final : wire::Synthesis::Service
{
  explicit Impl(TtsRpcInput input)
      : input_(std::move(input)), slots_(std::max(1, input_.slots))
  {
    if (!input_.synthesize || input_.capabilities.sampleRate <= 0 ||
        input_.capabilities.channels != 1 || input_.credentials.empty() ||
        !std::isfinite(input_.capabilities.defaultSpeed) ||
        input_.capabilities.defaultSpeed <= 0)
      throw std::invalid_argument("Invalid TTS RPC configuration");
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(32768);
    builder.AddListeningPort(input_.address, grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(this);
    server_ = builder.BuildAndStart();
    if (!server_)
      throw std::runtime_error("TTS RPC listener failed");
  }

  grpc::Status Capabilities(grpc::ServerContext* context,
                            const wire::CapabilitiesRequest*,
                            wire::CapabilitiesResponse* response) override
  {
    if (!authorized(*context, input_.credentials))
      return argus::response::toRpcStatus(ResponseException(401, TtsErrors::Unauthorized));
    response->set_sample_rate(input_.capabilities.sampleRate);
    response->set_channels(input_.capabilities.channels);
    response->set_format(wire::SAMPLE_FORMAT_FLOAT32);
    response->set_default_speed(input_.capabilities.defaultSpeed);
    for (const auto& voice : input_.capabilities.voices)
      response->add_voices(voice);
    for (const auto& language : input_.capabilities.languages)
      response->add_languages(language);
    return grpc::Status::OK;
  }

  grpc::Status Synthesize(grpc::ServerContext* context,
                          const wire::SynthesisRequest* request,
                          grpc::ServerWriter<wire::AudioChunk>* writer) override
  {
    if (!authorized(*context, input_.credentials))
      return argus::response::toRpcStatus(ResponseException(401, TtsErrors::Unauthorized));
    if (!validRequest(*request, input_.capabilities))
      return argus::response::toRpcStatus(ResponseException(400, TtsErrors::InvalidRequest));
    if (context->deadline() > Clock::now() + std::chrono::seconds(120))
      return argus::response::toRpcStatus(ResponseException(400, TtsErrors::InvalidRequest));
    if (context->IsCancelled() || Clock::now() >= context->deadline())
      return argus::response::toRpcStatus(stoppedError(*context));
    if (!slots_.try_acquire())
      return argus::response::toRpcStatus(ResponseException(429, TtsErrors::Busy));
    struct Release
    {
      std::counting_semaphore<>& slots;
      ~Release() { slots.release(); }
    } release{slots_};
    TtsLang language = TtsLang::EN;
    for (int index = 0; index < kTtsLangCount; ++index) {
      if (request->language() == langCode(static_cast<TtsLang>(index))) {
        language = static_cast<TtsLang>(index);
        break;
      }
    }
    TtsRequest synthesis{.text = request->text(),
                               .lang = language,
                               .voiceId = request->voice(),
                               .quality = static_cast<TtsQuality>(request->quality()),
                               .speed = request->speed() == 0
                                   ? input_.capabilities.defaultSpeed : request->speed()};
    StreamQueue queue;
    std::jthread producer([&, synthesis] {
      try {
        if (context->IsCancelled() || Clock::now() >= context->deadline())
          throw stoppedError(*context);
        input_.synthesize({.request = synthesis,
                           .onChunk = [&](const std::vector<float>& samples) {
                             for (size_t offset = 0;
                                  offset < samples.size(); offset += 4096) {
                               std::unique_lock lock(queue.mutex);
                               while (queue.chunks.size() >= 8 && !queue.stopped &&
                                      !context->IsCancelled() &&
                                      Clock::now() < context->deadline())
                                 queue.ready.wait_for(
                                     lock, std::chrono::milliseconds(10));
                               if (queue.stopped || context->IsCancelled() ||
                                   Clock::now() >= context->deadline())
                                 throw stoppedError(*context);
                               const auto end =
                                   std::min(samples.size(), offset + 4096);
                               queue.chunks.emplace_back(
                                   samples.begin() + offset,
                                   samples.begin() + end);
                               queue.ready.notify_all();
                             }
                           },
                           .stopRequested = [&] {
                             return context->IsCancelled() ||
                                    Clock::now() >= context->deadline();
                           }});
      }
      catch (const ResponseException& error) {
        std::lock_guard lock(queue.mutex);
        queue.status = argus::response::toRpcStatus(error);
      }
      catch (...) {
        std::lock_guard lock(queue.mutex);
        queue.status = argus::response::toRpcStatus(ResponseException(500, TtsErrors::InternalError));
      }
      std::lock_guard lock(queue.mutex);
      queue.done = true;
      queue.ready.notify_all();
    });
    std::uint64_t sequence = 0;
    for (;;) {
      std::unique_lock lock(queue.mutex);
      queue.ready.wait_for(lock, std::chrono::milliseconds(10), [&] {
        return queue.done || !queue.chunks.empty();
      });
      if (context->IsCancelled() || Clock::now() >= context->deadline()) {
        queue.stopped = true;
        queue.ready.notify_all();
        break;
      }
      if (queue.chunks.empty()) {
        if (queue.done)
          break;
        continue;
      }
      auto samples = std::move(queue.chunks.front());
      queue.chunks.pop_front();
      queue.ready.notify_all();
      lock.unlock();
      wire::AudioChunk chunk;
      chunk.set_sample_rate(input_.capabilities.sampleRate);
      chunk.set_channels(input_.capabilities.channels);
      chunk.set_format(wire::SAMPLE_FORMAT_FLOAT32);
      chunk.set_sequence(sequence++);
      chunk.mutable_samples()->Add(samples.begin(), samples.end());
      if (!writer->Write(chunk)) {
        std::lock_guard stoppedLock(queue.mutex);
        queue.stopped = true;
        queue.ready.notify_all();
        break;
      }
    }
    producer.join();
    if (queue.stopped)
      return argus::response::toRpcStatus(stoppedError(*context));
    if (queue.status.ok() && sequence == 0)
      return {grpc::StatusCode::INTERNAL, "No synthesis audio"};
    return queue.status;
  }

  TtsRpcInput input_;
  std::counting_semaphore<> slots_;
  int port_{0};
  std::unique_ptr<grpc::Server> server_;
};

TtsRpcServer::TtsRpcServer(TtsRpcInput input)
    : impl_(std::make_unique<Impl>(std::move(input)))
{
}

TtsRpcServer::~TtsRpcServer() { shutdown(); }
int TtsRpcServer::port() const { return impl_->port_; }
void TtsRpcServer::shutdown()
{
  if (impl_->server_) {
    impl_->server_->Shutdown(Clock::now());
    impl_->server_->Wait();
    impl_->server_.reset();
  }
}
