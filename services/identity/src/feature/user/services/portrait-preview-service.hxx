#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/user/dtos/response-portrait-preview-capability-dto.hxx>
#include <feature/user/dtos/response-portrait-preview-image-dto.hxx>
#include <feature/user/repositories/portrait-preview-capability/portrait-preview-capability-repository.hxx>
#include <shared/repositories/user/user-repository.hxx>
#include <shared/services/storage/private-portrait-service.hxx>
#include <string>
#include <auth/user-role.hxx>

struct PortraitPreviewCreateInput
{
  int64_t portraitUserId{0};
  int64_t requesterUserId{0};
  UserRole requesterRole{UserRole::Unknown};
};

struct PortraitPreviewConsumeInput
{
  std::string token;
  int64_t requesterUserId{0};
  UserRole requesterRole{UserRole::Unknown};
};

class PortraitPreviewService
{
public:
  drogon::Task<ResponsePortraitPreviewCapabilityDto>
  create(const PortraitPreviewCreateInput& input) const;
  drogon::Task<ResponsePortraitPreviewImageDto>
  consume(const PortraitPreviewConsumeInput& input) const;

private:
  static std::string hashToken(const std::string& token);
  static std::string newToken();
  static void requireAccess(UserRole role);

  PortraitPreviewCapabilityRepository capabilityRepository_;
  UserRepository userRepository_;
  PrivatePortraitService privatePortraitService_;
};
