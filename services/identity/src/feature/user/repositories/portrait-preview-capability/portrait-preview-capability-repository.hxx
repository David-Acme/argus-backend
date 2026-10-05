#pragma once

#include "portrait-preview-capability-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <feature/user/schemas/portrait-preview-capability/portrait-preview-capability-schema.hxx>

class PortraitPreviewCapabilityRepository
{
public:
  drogon::Task<PortraitPreviewCapabilitySchema>
  create(const PortraitPreviewCapabilityCreateInput& input) const;
  drogon::Task<std::optional<PortraitPreviewCapabilitySchema>>
  findByTokenHash(const std::string& tokenHash,
                  drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<bool>
  tryConsume(const PortraitPreviewCapabilityConsumeInput& input) const;
  drogon::Task<void> purgeSpent(int64_t now) const;
};
