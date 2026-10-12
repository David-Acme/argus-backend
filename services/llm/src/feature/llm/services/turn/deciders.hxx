#pragma once

#include "decider.hxx"

#include <phrase/phrase-catalog.hxx>

#include <vector>

namespace turn
{

class RuleDecider final : public Decider
{
public:
  [[nodiscard]] std::string_view id() const override { return kDeciderIds[0]; }

  [[nodiscard]] std::optional<Candidate> decide(const DecideInput& input) const override;
};

class TriggerDecider final : public Decider
{
public:
  TriggerDecider();

  [[nodiscard]] std::string_view id() const override { return kDeciderIds[1]; }

  [[nodiscard]] std::optional<Candidate> decide(const DecideInput& input) const override;

private:
  PhraseCatalog catalog_;
};

class FirstOf final : public Decider
{
public:
  explicit FirstOf(std::vector<const Decider*> deciders) : deciders_(std::move(deciders)) {}

  [[nodiscard]] std::string_view id() const override { return "stack"; }

  [[nodiscard]] std::optional<Candidate> decide(const DecideInput& input) const override;

private:
  std::vector<const Decider*> deciders_;
};

}
