#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <string>

// Typed spoken-name write input; role rides the x-argus-role metadata.
struct UpdateUserNameInput
{
  int64_t userId{0};
  std::string name;
  std::string role;
};

// Thin SDK wrapper over argus.identity.v1.IdentityService (rule 23).
class IdentityClient
{
public:
  explicit IdentityClient(std::string target);

  IdentityClient(const IdentityClient&) = delete;
  IdentityClient& operator=(const IdentityClient&) = delete;
  virtual ~IdentityClient() = default;

  // Spoken-name write; nullopt when the gateway refuses or is unreachable.
  virtual std::optional<argus::identity::v1::UserIdentity>
  updateUserName(const UpdateUserNameInput& input) const;

private:
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<argus::identity::v1::IdentityService::StubInterface>
      stub_;
};
