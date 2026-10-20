#include <doctest/doctest.h>

#include <feature/messenger/infra/identity-household-roster.hxx>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace
{

struct Member
{
  int64_t id{0};
  std::string name;
  std::string role;
  bool active{false};
  bool hasRole{true};
};

argus::identity::v1::UserIdentity userOf(const Member& member)
{
  argus::identity::v1::UserIdentity user;
  user.set_user_id(member.id);
  user.set_name(member.name);
  if (member.hasRole)
    user.set_role(member.role);
  user.set_is_active(member.active);
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
      userOf({.id = 1, .name = "David Acme", .role = "owner", .active = true, .hasRole = true}),
      userOf({.id = 2, .name = "Ana Lopez", .role = "resident", .active = true, .hasRole = true}),
      userOf({.id = 3, .name = "Sin rol", .role = "", .active = true, .hasRole = false}),
      userOf({.id = 4, .name = "Lucia", .role = "guest", .active = false, .hasRole = true}),
      userOf({.id = 5, .name = "Pedro", .role = "superuser", .active = true, .hasRole = true}),
      userOf({.id = 6, .name = "Guardia", .role = "guard", .active = true, .hasRole = true}),
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
