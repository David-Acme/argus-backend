#include "guard-duress-link.hxx"

#include <guard/guard-duress-client.hxx>

#include <utility>

GuardDuressLink::GuardDuressLink(std::shared_ptr<const GuardDuressClient> client) : client_(std::move(client)) {}

std::optional<std::string> GuardDuressLink::phraseKey() const
{
  return client_->phraseKey();
}

DuressDelivery GuardDuressLink::raise(const DuressRaise& raise) const
{
  switch (client_->raiseSpoken(
      {.userId = raise.userId, .role = userRoleToString(raise.role), .key = raise.key, .saidAt = raise.saidAt})) {
    case SpokenDuressReply::Raised:
    case SpokenDuressReply::Repeated:
      return DuressDelivery::Delivered;
    case SpokenDuressReply::Stale:
    case SpokenDuressReply::NotAllowed:
      return DuressDelivery::Dropped;
    case SpokenDuressReply::Unavailable:
      return DuressDelivery::Retry;
  }
  return DuressDelivery::Retry;
}
