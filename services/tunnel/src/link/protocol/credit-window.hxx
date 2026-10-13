#pragma once

#include <link/protocol/frames.hxx>

#include <cstddef>
#include <cstdint>

namespace tunnel::link
{
class CreditWindow
{
public:
  struct Params
  {
    bool limited{false};
    uint32_t window{0};
    uint32_t grantThreshold{kCreditGrantThreshold};
  };

  CreditWindow() = default;
  explicit CreditWindow(const Params& params);

  [[nodiscard]] bool limited() const noexcept { return limited_; }
  [[nodiscard]] uint32_t remaining() const noexcept { return remaining_; }
  [[nodiscard]] bool covers(size_t bytes) const noexcept;
  void recordUsed(size_t bytes) noexcept;
  void grant(uint32_t bytes) noexcept;
  void markConsumed(size_t bytes) noexcept;
  uint32_t takePendingGrant() noexcept;

private:
  bool limited_{false};
  uint32_t window_{0};
  uint32_t remaining_{0};
  uint32_t consumed_{0};
  uint32_t grantThreshold_{kCreditGrantThreshold};
};
}
