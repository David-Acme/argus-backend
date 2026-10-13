#pragma once

#include <feature/duress/repositories/duress-raise/duress-raise-query.hxx>

#include <cstdint>
#include <optional>
#include <vector>

class DuressOutbox
{
public:
  virtual ~DuressOutbox() = default;

  [[nodiscard]] virtual std::optional<int64_t> add(const DuressRaiseCreateInput& input) = 0;

  [[nodiscard]] virtual std::vector<DuressRaiseRow> pending() = 0;

  virtual void remove(int64_t id) = 0;
};
