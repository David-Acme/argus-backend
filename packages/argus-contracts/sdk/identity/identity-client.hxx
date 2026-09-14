#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// Typed spoken-name write input; role rides the x-argus-role metadata.
struct UpdateUserNameInput
{
  int64_t userId{0};
  std::string name;
  std::string role;
};

// Token validation input; presence and value of the device context carried separately.
struct ValidateTokenInput
{
  std::string accessToken;
  std::string deviceHash;
  bool hasDeviceContext{false};
};

// Camera-guard enrollment of an unknown face; snapshot is the JPEG crop.
struct EnrollPersonInput
{
  std::string image;
  int64_t cameraId{0};
  bool captureSnapshot{false};
};

// Camera-guard tags and observation for a person.
struct TagPersonInput
{
  int64_t personId{0};
  std::vector<std::string> tags;
  std::string source{"llm"};
  std::string observation;
};

// Person identity plus accumulated tags, for the guard agent context.
struct PersonProfile
{
  int64_t personId{0};
  std::optional<int64_t> userId;
  std::string name;
  std::string alias;
  std::string observation;
  std::string role;
  std::vector<std::string> tags;
};

// Owner-approved promotion; accessToken is the owner's bearer token and
// deviceHash binds the call to the owner's active session when present.
struct PromotePersonInput
{
  int64_t personId{0};
  std::string accessToken;
  std::string deviceHash;
};

// Thin SDK wrapper over argus.identity.v1.IdentityService (rule 23).
class IdentityClient
{
public:
  // fleetSecret rides every call as x-argus-fleet.
  explicit IdentityClient(std::string target, std::string fleetSecret = {});

  IdentityClient(const IdentityClient&) = delete;
  IdentityClient& operator=(const IdentityClient&) = delete;
  virtual ~IdentityClient() = default;

  // Spoken-name write; nullopt when the gateway refuses or is unreachable.
  virtual std::optional<argus::identity::v1::UserIdentity>
  updateUserName(const UpdateUserNameInput& input) const;

  // Server-authoritative token validation; nullopt when the gateway is unreachable.
  virtual std::optional<argus::identity::v1::ValidateTokenResponse>
  validateToken(const ValidateTokenInput& input) const;

  // Typed directory read; nullopt when unknown or unreachable.
  virtual std::optional<argus::identity::v1::GetUserResponse>
  getUser(int64_t userId) const;

  // Catalog snapshot read; nullopt when the gateway is unreachable.
  virtual std::optional<argus::identity::v1::ListPersonsResponse>
  listPersons() const;

  // Device credential check by secret hash; false when unknown or unreachable.
  virtual bool checkDeviceCredential(const std::string& secretHash) const;

  // Face crop identification; nullopt when no face, no match or unreachable.
  virtual std::optional<argus::identity::v1::IdentifyPersonResponse>
  identifyPerson(const std::string& image) const;

  // Unknown-face enrollment; nullopt when unreachable.
  virtual std::optional<argus::identity::v1::EnrollPersonResponse>
  enrollPerson(const EnrollPersonInput& input) const;

  // last_seen_at bump; false when unknown or unreachable.
  virtual bool touchPerson(int64_t personId, int64_t at) const;

  // Candidate promotion to trusted; the server verifies the owner token.
  virtual bool promotePerson(const PromotePersonInput& input) const;

  // LLM tags/observation append; false when unreachable.
  virtual bool tagPerson(const TagPersonInput& input) const;

  // Prior tags for one person; nullopt when unreachable.
  virtual std::optional<std::vector<std::string>> personTags(
      int64_t personId) const;

  // Full person view (identity + tags); nullopt when unknown or unreachable.
  virtual std::optional<PersonProfile> getPerson(int64_t personId) const;

  // Owner+guard ids for security notifications; nullopt when unreachable.
  virtual std::optional<std::vector<int64_t>> listNotifiableUsers() const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::identity::v1::IdentityService::StubInterface>
      stub_;
  std::string fleetSecret_;
};
