#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <shared/repositories/device-credential/device-credential-repository.hxx>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <shared/repositories/person-snapshot/person-snapshot-repository.hxx>
#include <shared/repositories/person-tag/person-tag-repository.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/refresh-token/refresh-token-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/jwt/jwt-service.hxx>
#include <shared/services/sync-audit/sync-audit-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

// IdentityService controller: UpdateUser is metadata-authoritative, the rest self-authoritative.
class IdentityRpcService final
    : public argus::identity::v1::IdentityService::CallbackService
{
public:
  // fleetSecret is required in x-argus-fleet on every call; empty allows only loopback.
  IdentityRpcService(std::shared_ptr<NatsBus> bus, std::string fleetSecret);

  grpc::ServerUnaryReactor*
  UpdateUser(grpc::CallbackServerContext* context,
             const argus::identity::v1::UpdateUserRequest* request,
             argus::identity::v1::UpdateUserResponse* response) override;

  grpc::ServerUnaryReactor*
  ValidateToken(grpc::CallbackServerContext* context,
                const argus::identity::v1::ValidateTokenRequest* request,
                argus::identity::v1::ValidateTokenResponse* response) override;

  grpc::ServerUnaryReactor*
  GetUser(grpc::CallbackServerContext* context,
          const argus::identity::v1::GetUserRequest* request,
          argus::identity::v1::GetUserResponse* response) override;

  grpc::ServerUnaryReactor*
  ListPersons(grpc::CallbackServerContext* context,
              const argus::identity::v1::ListPersonsRequest* request,
              argus::identity::v1::ListPersonsResponse* response) override;

  grpc::ServerUnaryReactor*
  CheckDeviceCredential(
      grpc::CallbackServerContext* context,
      const argus::identity::v1::CheckDeviceCredentialRequest* request,
      argus::identity::v1::CheckDeviceCredentialResponse* response) override;

  grpc::ServerUnaryReactor*
  IdentifyPerson(grpc::CallbackServerContext* context,
                 const argus::identity::v1::IdentifyPersonRequest* request,
                 argus::identity::v1::IdentifyPersonResponse* response) override;

  grpc::ServerUnaryReactor*
  EnrollPerson(grpc::CallbackServerContext* context,
               const argus::identity::v1::EnrollPersonRequest* request,
               argus::identity::v1::EnrollPersonResponse* response) override;

  grpc::ServerUnaryReactor*
  TouchPerson(grpc::CallbackServerContext* context,
              const argus::identity::v1::TouchPersonRequest* request,
              argus::identity::v1::TouchPersonResponse* response) override;

  grpc::ServerUnaryReactor*
  TagPerson(grpc::CallbackServerContext* context,
            const argus::identity::v1::TagPersonRequest* request,
            argus::identity::v1::TagPersonResponse* response) override;

  grpc::ServerUnaryReactor*
  GetPersonTags(grpc::CallbackServerContext* context,
                const argus::identity::v1::PersonTagsRequest* request,
                argus::identity::v1::PersonTagsResponse* response) override;

  grpc::ServerUnaryReactor*
  GetPerson(grpc::CallbackServerContext* context,
            const argus::identity::v1::GetPersonRequest* request,
            argus::identity::v1::GetPersonResponse* response) override;

  grpc::ServerUnaryReactor*
  PromotePerson(grpc::CallbackServerContext* context,
                const argus::identity::v1::PromotePersonRequest* request,
                argus::identity::v1::PromotePersonResponse* response) override;

  grpc::ServerUnaryReactor*
  ListNotifiableUsers(
      grpc::CallbackServerContext* context,
      const argus::identity::v1::ListNotifiableUsersRequest* request,
      argus::identity::v1::ListNotifiableUsersResponse* response) override;

private:
  struct TokenRejectionInput
  {
    grpc::ServerUnaryReactor* reactor;
    argus::identity::v1::ValidateTokenResponse* response;
    const std::string& reason;
  };

  // Rejected validation: RPC OK with valid=false and the caller's 401 body.
  static void finishRejected(const TokenRejectionInput& input);

  // Fleet-secret gate; the listener is cleartext.
  bool fleetAuthorized(const grpc::CallbackServerContext* context) const;

  JwtService jwtService_;
  UserRepository userRepository_;
  PersonRepository personRepository_;
  FaceEmbeddingRepository faceEmbeddingRepository_;
  PersonTagRepository personTagRepository_;
  PersonSnapshotRepository personSnapshotRepository_;
  RefreshTokenRepository refreshTokenRepository_;
  DeviceCredentialRepository deviceCredentialRepository_;
  SyncAuditService auditService_;
  std::shared_ptr<NatsBus> bus_;
  std::string fleetSecret_;
};
