#pragma once

#include "slots.hxx"

#include <feature/memory/services/extract/extraction-service.hxx>

namespace turn
{

class ModelText final : public slots::TextSlots
{
public:
  explicit ModelText(ExtractionService& model) : model_(model) {}

  [[nodiscard]] std::optional<std::string> extract(const slots::TextRequest& request) const override;

private:
  ExtractionService& model_;
  slots::RuleText rules_;
};

}
