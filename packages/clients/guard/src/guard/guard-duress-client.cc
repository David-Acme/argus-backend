#include "guard-duress-client.hxx"

#include <grpc/grpc-client-base.hxx>
#include <utility>

namespace
{
SpokenDuressReply replyOf(argus::guard::v1::SpokenDuressOutcome outcome)
{
  switch (outcome) {
    case argus::guard::v1::SPOKEN_DURESS_OUTCOME_RAISED:
      return SpokenDuressReply::Raised;
    case argus::guard::v1::SPOKEN_DURESS_OUTCOME_REPEATED:
      return SpokenDuressReply::Repeated;
    case argus::guard::v1::SPOKEN_DURESS_OUTCOME_STALE:
      return SpokenDuressReply::Stale;
    case argus::guard::v1::SPOKEN_DURESS_OUTCOME_NOT_ALLOWED:
      return SpokenDuressReply::NotAllowed;
    default:
      return SpokenDuressReply::Unavailable;
  }
}
}

GuardDuressClient::GuardDuressClient(GuardDuressClientConfig config)
    : channel_(argus::client::makeChannel(config.target)),
      stub_(argus::guard::v1::DuressService::NewStub(channel_)),
      credential_(std::move(config.credential))
{
}

std::optional<std::string> GuardDuressClient::phraseKey() const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);
  const argus::guard::v1::DuressPhraseRequest request;
  argus::guard::v1::DuressPhraseResponse response;
  if (const grpc::Status status = stub_->GetPhrase(&context, request, &response); !status.ok())
    return std::nullopt;
  return response.key();
}

SpokenDuressReply GuardDuressClient::raiseSpoken(const SpokenDuressCall& call) const
{
  grpc::ClientContext context;
  argus::client::setDeadline(context, kCallTimeoutMs);
  argus::client::addCallerCredential(context, credential_);
  argus::guard::v1::SpokenDuressRequest request;
  request.set_user_id(call.userId);
  request.set_role(call.role);
  request.set_key(call.key);
  argus::guard::v1::SpokenDuressResponse response;
  if (const grpc::Status status = stub_->RaiseSpoken(&context, request, &response); !status.ok())
    return SpokenDuressReply::Unavailable;
  return replyOf(response.outcome());
}
