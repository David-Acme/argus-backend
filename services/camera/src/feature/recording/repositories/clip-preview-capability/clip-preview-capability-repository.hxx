#pragma once

#include "clip-preview-capability-query.hxx"

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <feature/recording/schemas/clip-preview-capability/clip-preview-capability-schema.hxx>
#include <optional>

class ClipPreviewCapabilityRepository
{
public:
  drogon::Task<ClipPreviewCapabilitySchema>
  create(const ClipPreviewCapabilityCreateInput& input) const;
  drogon::Task<std::optional<ClipPreviewCapabilitySchema>>
  findByTokenHash(const std::string& tokenHash,
                  drogon::orm::DbClient* client = nullptr) const;
  drogon::Task<bool> tryConsume(const ClipPreviewCapabilityConsumeInput& input) const;
  drogon::Task<void> purgeSpent(int64_t now) const;
};
