#pragma once

#include <feature/duress/services/duress-link.hxx>
#include <shared/vocabulary/utterance-screen.hxx>

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <stop_token>
#include <thread>

struct DuressScreenState;

struct DuressScreenDependencies
{
  std::shared_ptr<const DuressLink> link;
  std::function<bool(UserRole)> eligible;
};

struct DuressScreenTiming
{
  std::chrono::milliseconds refresh{std::chrono::minutes(1)};
  std::chrono::milliseconds retryBase{std::chrono::seconds(2)};
  std::chrono::milliseconds retryMax{std::chrono::minutes(1)};
  std::chrono::milliseconds memo{std::chrono::minutes(2)};
  std::size_t queueLimit{32};
};

class DuressScreen final : public turn::UtteranceScreen
{
public:
  DuressScreen(DuressScreenDependencies dependencies, DuressScreenTiming timing);
  ~DuressScreen() override;

  DuressScreen(const DuressScreen&) = delete;
  DuressScreen& operator=(const DuressScreen&) = delete;
  DuressScreen(DuressScreen&&) = delete;
  DuressScreen& operator=(DuressScreen&&) = delete;

  [[nodiscard]] turn::ScreenedText screen(const turn::ScreenedUtterance& utterance) const override;

  void start();
  void requestRefresh() const;
  void requestStop();
  [[nodiscard]] bool drained() const;
  [[nodiscard]] bool idle() const;

private:
  void run(const std::stop_token& stop) const;

  DuressScreenDependencies dependencies_;
  DuressScreenTiming timing_;
  std::shared_ptr<DuressScreenState> state_;
  std::jthread worker_;
};
