#pragma once

#include <auth/user-role.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/person/repositories/person-tag/person-tag-repository.hxx>
#include <feature/visitor/repositories/visitor/visitor-repository.hxx>
#include <feature/visitor/services/visit-pattern.hxx>
#include <identity/person-category.hxx>
#include <optional>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/privacy/privacy-gate.hxx>
#include <string>
#include <vector>

struct PersonTagRequest
{
  int64_t personId{0};
  std::vector<std::string> tags;
  std::string source;
  std::string observation;
};

struct PersonPromoteRequest
{
  int64_t personId{0};
  int64_t actorId{0};
};

struct HouseholdMatchRequest
{
  int64_t personId{0};
  bool forCamera{false};
};

struct HouseholdMatch
{
  bool trusted{false};
  bool accountDisabled{false};
  std::optional<UserRole> role;
  std::optional<int64_t> userId;
  std::string name;
  std::string lastName;
};

struct PersonDescription
{
  int64_t personId{0};
  std::optional<int64_t> userId;
  bool named{false};
  std::string name;
  std::string alias;
  std::string observation;
  bool trusted{false};
  std::optional<UserRole> role;
  bool visitor{false};
  PersonCategory category{PersonCategory::None};
  int64_t visits{0};
  int64_t firstSeenAt{0};
  int64_t lastSeenAt{0};
  std::optional<int64_t> visitorNumber;
  VisitPatternSummary pattern;
  std::vector<std::string> tags;
};

class PersonFeatureService
{
public:
  [[nodiscard]] drogon::Task<bool> touch(int64_t personId, int64_t at) const;
  [[nodiscard]] drogon::Task<int> tag(const PersonTagRequest& request) const;
  [[nodiscard]] drogon::Task<std::vector<std::string>> tags(int64_t personId) const;
  [[nodiscard]] drogon::Task<std::optional<PersonDescription>>
  describe(int64_t personId) const;
  [[nodiscard]] drogon::Task<HouseholdMatch>
  describeMatch(const HouseholdMatchRequest& request) const;
  [[nodiscard]] drogon::Task<std::optional<bool>>
  promote(const PersonPromoteRequest& request) const;

private:
  PersonRepository personRepository_;
  PersonTagRepository tagRepository_;
  UserRepository userRepository_;
  VisitorRepository visitorRepository_;
  PrivacyGate privacyGate_;
};
