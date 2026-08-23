#pragma once

#include "portrait-preview-capability-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/portrait-preview-capability/portrait-preview-capability-schema.hxx>

class PortraitPreviewCapabilityRepository
{
public:
  drogon::Task<PortraitPreviewCapabilitySchema>
  create(const PortraitPreviewCapabilityCreateInput& input) const;
  drogon::Task<std::optional<PortraitPreviewCapabilitySchema>>
  findByTokenHash(const std::string& tokenHash) const;
  drogon::Task<bool>
  tryConsume(const PortraitPreviewCapabilityConsumeInput& input) const;
};
