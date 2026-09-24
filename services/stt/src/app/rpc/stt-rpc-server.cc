#include "stt-rpc-server.hxx"

#include <errors/response-exception.hxx>
#include <grpc/grpc-server-identity.hxx>
#include <response/response-rpc.hxx>
#include <stt.grpc.pb.h>
#include <stt/stt-errors.hxx>
#include <algorithm>
#include <chrono>
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

ResponseException stoppedError(const grpc::ServerContext& context)
{
  if (Clock::now() >= context.deadline())
    return {504, SttErrors::DeadlineExceeded};
  return {499, SttErrors::Cancelled};
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

bool validRequest(const wire::TranscribeRequest& request,
                  const std::function<bool(const std::string&)>& acceptsLanguage)
{
  return !request.samples().empty() &&
         request.sample_rate() >= static_cast<std::uint32_t>(kMinSampleRate) &&
         request.sample_rate() <= static_cast<std::uint32_t>(kMaxSampleRate) &&
         (request.language().empty() || acceptsLanguage(request.language()));
}
}

struct SttRpcServer::Impl final : wire::Transcription::Service
{
  explicit Impl(SttRpcInput input)
      : input_(std::move(input)), slots_(std::max(1, input_.slots))
  {
    if (!input_.transcribe || !input_.capabilities || !input_.acceptsLanguage ||
        input_.credentials.empty())
      throw std::invalid_argument("Invalid STT RPC configuration");
    grpc::ServerBuilder builder;
    builder.SetMaxReceiveMessageSize(kMaxReceiveBytes);
    builder.AddListeningPort(input_.address, grpc::InsecureServerCredentials(), &port_);
    builder.RegisterService(this);
    server_ = builder.BuildAndStart();
    if (!server_)
      throw std::runtime_error("STT RPC listener failed");
  }

  grpc::Status Capabilities(grpc::ServerContext* context,
                            const wire::CapabilitiesRequest*,
                            wire::CapabilitiesResponse* response) override
  {
    if (!authorized(*context, input_.credentials))
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
    if (!authorized(*context, input_.credentials))
      return argus::response::toRpcStatus(ResponseException(401, SttErrors::Unauthorized));
    try {
      if (!validRequest(*request, input_.acceptsLanguage))
        return argus::response::toRpcStatus(ResponseException(400, SttErrors::InvalidRequest));
      if (context->deadline() != Clock::time_point::max() &&
          context->deadline() > Clock::now() + kMaxDeadline)
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

  SttRpcInput input_;
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
