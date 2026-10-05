#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

struct TapoTrustState
{
  std::string fingerprint;
  bool secure{false};
};

class TapoTrust
{
public:
  using Persist = std::function<void(const TapoTrustState&)>;

  TapoTrust(TapoTrustState initial, Persist persist);

  [[nodiscard]] TapoTrustState state() const;
  [[nodiscard]] std::string pin() const;
  [[nodiscard]] bool secureSeen() const;

  void learn(std::string_view fingerprint);
  void noteSecure();

private:
  mutable std::mutex mutex_;
  TapoTrustState state_;
  Persist persist_;
};
