#include <doctest/doctest.h>

#include <feature/messenger/infra/identity-household-roster.hxx>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{

argus::identity::v1::UserIdentity userOf(int64_t id, const std::string& name, const std::string& role, bool active,
                                         bool hasRole = true)
{
  argus::identity::v1::UserIdentity user;
  user.set_user_id(id);
  user.set_name(name);
  if (hasRole)
    user.set_role(role);
  user.set_is_active(active);
  return user;
}

class FakeIdentity final : public IdentityClient
{
public:
  FakeIdentity()
      : IdentityClient("127.0.0.1:1", argus::client::PeerCredential{.credential = {}, .fleetSecret = {}})
  {
  }

  [[nodiscard]] std::optional<std::vector<argus::identity::v1::UserIdentity>> listUsers() const override
  {
    return answer;
  }

  std::optional<std::vector<argus::identity::v1::UserIdentity>> answer;
};

}

TEST_CASE("the roster is the active household members with a role this build knows, and nobody else")
{
  auto identity = std::make_shared<FakeIdentity>();
  identity->answer = std::vector<argus::identity::v1::UserIdentity>{
      userOf(1, "David Acme", "owner", true),
      userOf(2, "Ana Lopez", "resident", true),
      userOf(3, "Sin rol", "", true, false),
      userOf(4, "Lucia", "guest", false),
      userOf(5, "Pedro", "superuser", true),
      userOf(6, "Guardia", "guard", true),
  };
  const IdentityHouseholdRoster roster(identity);
  const auto members = roster.members();
  REQUIRE(members.size() == 3);
  CHECK(members[0].userId == 1);
  CHECK(members[0].name == "David Acme");
  CHECK(members[1].userId == 2);
  CHECK(members[2].userId == 6);
}

TEST_CASE("an unavailable identity gives an empty roster, so no name resolves")
{
  auto identity = std::make_shared<FakeIdentity>();
  identity->answer = std::nullopt;
  const IdentityHouseholdRoster roster(identity);
  CHECK(roster.members().empty());
}

TEST_CASE("without an identity client the roster is empty")
{
  const IdentityHouseholdRoster roster(nullptr);
  CHECK(roster.members().empty());
}
