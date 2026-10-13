#include <link/protocol/credit-window.hxx>

#include <algorithm>

namespace tunnel::link
{
CreditWindow::CreditWindow(const Params& params)
    : limited_{params.limited},
      window_{params.window},
      remaining_{params.window},
      grantThreshold_{params.grantThreshold == 0 ? kCreditGrantThreshold
                                                 : params.grantThreshold}
{
}

bool CreditWindow::covers(size_t bytes) const noexcept
{
  if (!limited_)
    return true;
  return bytes <= remaining_;
}

void CreditWindow::recordUsed(size_t bytes) noexcept
{
  if (!limited_)
    return;
  const auto used = static_cast<uint32_t>(std::min<size_t>(bytes, remaining_));
  remaining_ -= used;
}

void CreditWindow::grant(uint32_t bytes) noexcept
{
  if (!limited_)
    return;
  const uint64_t total = static_cast<uint64_t>(remaining_) + bytes;
  remaining_ = static_cast<uint32_t>(std::min<uint64_t>(total, window_));
}

void CreditWindow::markConsumed(size_t bytes) noexcept
{
  if (!limited_)
    return;
  const uint64_t total = static_cast<uint64_t>(consumed_) + bytes;
  consumed_ = static_cast<uint32_t>(std::min<uint64_t>(total, window_));
}

uint32_t CreditWindow::takePendingGrant() noexcept
{
  if (!limited_ || consumed_ < grantThreshold_)
    return 0;
  const uint32_t granted = consumed_;
  consumed_ = 0;
  grant(granted);
  return granted;
}
}
