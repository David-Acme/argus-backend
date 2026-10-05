#include "identity-presence-directory.hxx"

#include <identity/identity-client.hxx>

namespace
{
bool consents(const argus::identity::v1::PrivacyChoices& choices)
{
  return choices.decided() && choices.presence();
}
}

IdentityPresenceDirectory::IdentityPresenceDirectory(
    const IdentityClient* identity)
    : identity_(identity)
{
}

std::optional<bool>
IdentityPresenceDirectory::presenceConsent(int64_t userId) const
{
  if (identity_ == nullptr)
    return std::nullopt;
  const auto answer = identity_->getUser(userId);
  if (!answer)
    return std::nullopt;
  if (!answer->has_user() || answer->user().user_id() != userId)
    return false;
  const auto& user = answer->user();
  if (user.has_is_active() && !user.is_active())
    return false;
  return user.has_privacy() && consents(user.privacy());
}

std::optional<int64_t>
IdentityPresenceDirectory::userOfPerson(int64_t personId) const
{
  if (identity_ == nullptr)
    return std::nullopt;
  const auto person = identity_->getPerson(personId);
  if (!person || !person->userId || *person->userId <= 0)
    return std::nullopt;
  return person->userId;
}

std::optional<std::vector<int64_t>>
IdentityPresenceDirectory::consentingUsers() const
{
  if (identity_ == nullptr)
    return std::nullopt;
  const auto answer = identity_->listPrivacy();
  if (!answer)
    return std::nullopt;
  std::vector<int64_t> users;
  users.reserve(static_cast<size_t>(answer->users_size()));
  for (const auto& entry : answer->users())
    if (entry.has_choices() && consents(entry.choices()))
      users.push_back(entry.user_id());
  return users;
}
