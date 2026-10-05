#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

struct UpdateUserNameInput
{
  int64_t userId{0};
  std::string name;
  std::string role;
};

struct RegisterUserInput
{
  std::string image;
  std::string name;
  std::string invitationToken;
  std::string lang;
  std::string deviceHash;
};

struct EnrollPersonInput
{
  std::string image;
  int64_t cameraId{0};
  bool captureSnapshot{false};
};

struct TagPersonInput
{
  int64_t personId{0};
  std::vector<std::string> tags;
  std::string source{"llm"};
  std::string observation;
};

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

struct PromotePersonInput
{
  int64_t personId{0};
  std::string accessToken;
  std::string deviceHash;
};

class IdentityClient
{
public:
  explicit IdentityClient(std::string target, std::string fleetSecret = {});

  IdentityClient(const IdentityClient&) = delete;
  IdentityClient& operator=(const IdentityClient&) = delete;
  virtual ~IdentityClient() = default;

  virtual std::optional<argus::identity::v1::UserIdentity>
  updateUserName(const UpdateUserNameInput& input) const;

  virtual std::optional<argus::identity::v1::RegisterUserResponse>
  registerUser(const RegisterUserInput& input) const;

  virtual std::optional<argus::identity::v1::GetUserResponse>
  getUser(int64_t userId) const;

  virtual std::optional<argus::identity::v1::ListPersonsResponse>
  listPersons() const;

  virtual std::optional<argus::identity::v1::IdentifyPersonResponse>
  identifyPerson(const std::string& image) const;

  [[nodiscard]] virtual std::optional<
      argus::identity::v1::IdentifyPersonResponse>
  identifyForCamera(const std::string& image) const;

  virtual std::optional<argus::identity::v1::EnrollPersonResponse>
  enrollPerson(const EnrollPersonInput& input) const;

  virtual bool touchPerson(int64_t personId, int64_t at) const;

  virtual bool promotePerson(const PromotePersonInput& input) const;

  virtual bool tagPerson(const TagPersonInput& input) const;

  virtual std::optional<std::vector<std::string>> personTags(
      int64_t personId) const;

  virtual std::optional<PersonProfile> getPerson(int64_t personId) const;

  virtual std::optional<std::vector<int64_t>> listNotifiableUsers() const;

  [[nodiscard]] virtual std::optional<argus::identity::v1::ListPrivacyResponse>
  listPrivacy() const;

  [[nodiscard]] virtual std::optional<
      std::vector<argus::identity::v1::UserIdentity>>
  listUsers() const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::identity::v1::IdentityService::StubInterface>
      stub_;
  std::string fleetSecret_;
};
