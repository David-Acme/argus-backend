#pragma once

#include <feature/duress/services/duress-link.hxx>

#include <memory>

class GuardDuressClient;

class GuardDuressLink final : public DuressLink
{
public:
  explicit GuardDuressLink(std::shared_ptr<const GuardDuressClient> client);

  [[nodiscard]] std::optional<std::string> phraseKey() const override;

  [[nodiscard]] DuressDelivery raise(const DuressRaise& raise) const override;

private:
  std::shared_ptr<const GuardDuressClient> client_;
};
