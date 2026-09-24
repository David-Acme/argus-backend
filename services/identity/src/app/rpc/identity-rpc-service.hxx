#pragma once

#include <argus/identity/v1/identity.grpc.pb.h>
#include <auth/auth-client.hxx>
#include <grpcpp/grpcpp.h>
#include <memory>
#include <nats/nats-bus.hxx>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <shared/repositories/person-snapshot/person-snapshot-repository.hxx>
#include <shared/repositories/person-tag/person-tag-repository.hxx>
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <feature/enrollment/services/enrollment-feature-service.hxx>

class IdentityRpcService final
    : public argus::identity::v1::IdentityService::CallbackService
{
public:
  struct Dependencies
  {
    std::shared_ptr<NatsBus> bus;
    std::string fleetSecret;
    std::shared_ptr<const AuthClient> auth;
  };

  explicit IdentityRpcService(Dependencies dependencies);

  grpc::ServerUnaryReactor*
  UpdateUser(grpc::CallbackServerContext* context,
             const argus::identity::v1::UpdateUserRequest* request,
             argus::identity::v1::UpdateUserResponse* response) override;

  grpc::ServerUnaryReactor*
  RegisterUser(grpc::CallbackServerContext* context,
               const argus::identity::v1::RegisterUserRequest* request,
               argus::identity::v1::RegisterUserResponse* response) override;

  grpc::ServerUnaryReactor*
  GetUser(grpc::CallbackServerContext* context,
          const argus::identity::v1::GetUserRequest* request,
          argus::identity::v1::GetUserResponse* response) override;

  grpc::ServerUnaryReactor*
  ListPersons(grpc::CallbackServerContext* context,
              const argus::identity::v1::ListPersonsRequest* request,
              argus::identity::v1::ListPersonsResponse* response) override;

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
  bool fleetAuthorized(const grpc::CallbackServerContext* context) const;

  Dependencies dependencies_;
  EnrollmentFeatureService enrollmentService_;
  UserRepository userRepository_;
  PersonRepository personRepository_;
  FaceEmbeddingRepository faceEmbeddingRepository_;
  PersonTagRepository personTagRepository_;
  PersonSnapshotRepository personSnapshotRepository_;
};
