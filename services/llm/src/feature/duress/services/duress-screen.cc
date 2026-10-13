#include "duress-screen.hxx"

#include <llm/llm-speech.hxx>
#include <text/secret-phrase.hxx>
#include <text/sha256.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct DuressScreenState
{
  using Clock = std::chrono::steady_clock;
  using Fingerprint = std::array<std::uint8_t, 32>;

  enum class KeyState : uint8_t
  {
    Unknown,
    Absent,
    Present
  };

  struct Late
  {
    int64_t userId{0};
    UserRole role{UserRole::Unknown};
    std::string text;
    int64_t heardAt{0};
  };

  struct Pending
  {
    int64_t row{0};
    int64_t userId{0};
    UserRole role{UserRole::Unknown};
    std::string keyDigest;
    int64_t saidAt{0};
    int failures{0};
    Clock::time_point nextAt{};
  };

  struct Memo
  {
    Fingerprint fingerprint{};
    Clock::time_point at{};
  };

  mutable std::mutex mutex;
  std::condition_variable_any wake;
  std::deque<Late> late;
  std::vector<Pending> pending;
  std::unordered_map<int64_t, Memo> clean;
  KeyState keyState{KeyState::Unknown};
  std::optional<secret_phrase::PhraseKey> key;
  std::string encodedKey;
  std::string keyDigest;
  bool refreshRequested{true};
  Clock::time_point nextRefresh{};
  int refreshFailures{0};
  bool busy{false};
  std::atomic<bool> stopped{false};
  std::atomic<bool> finished{true};
};

namespace
{
using Clock = DuressScreenState::Clock;
using KeyState = DuressScreenState::KeyState;

DuressScreenState::Fingerprint fingerprintOf(std::string_view text)
{
  argus::hash::Sha256 hasher;
  hasher.update(text);
  return hasher.digest();
}

int64_t epochSeconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct BackoffInput
{
  const DuressScreenTiming& timing;
  int failures{0};
};

std::chrono::milliseconds backoff(const BackoffInput& input)
{
  std::chrono::milliseconds delay = input.timing.retryBase;
  for (int doubling = 1; doubling < input.failures && delay < input.timing.retryMax; ++doubling)
    delay *= 2;
  return std::min(delay, input.timing.retryMax);
}

bool waiting(const DuressScreenState& state, int64_t userId)
{
  return std::ranges::any_of(state.pending,
                             [userId](const DuressScreenState::Pending& entry) { return entry.userId == userId; });
}

void queueRaise(DuressScreenState& state, DuressScreenState::Pending entry)
{
  if (!waiting(state, entry.userId))
    state.pending.push_back(std::move(entry));
}

bool raiseDue(const DuressScreenState& state)
{
  if (state.keyState == KeyState::Unknown)
    return false;
  const Clock::time_point now = Clock::now();
  return std::ranges::any_of(state.pending,
                             [now](const DuressScreenState::Pending& entry) { return entry.nextAt <= now; });
}

bool sameKey(const DuressScreenState& state, const DuressScreenState::Pending& entry)
{
  return state.keyState == KeyState::Present && state.key && state.keyDigest == entry.keyDigest;
}

struct AdoptInput
{
  DuressScreenState& state;
  const DuressScreenTiming& timing;
  const std::optional<std::string>& fetched;
  Clock::time_point now;
};

void adoptKey(const AdoptInput& input)
{
  DuressScreenState& state = input.state;
  if (!input.fetched) {
    ++state.refreshFailures;
    if (state.refreshFailures == 1)
      LOG_WARN << "Duress screen: guard did not answer; the phrase is asked again";
    state.nextRefresh = input.now + backoff({.timing = input.timing, .failures = state.refreshFailures});
    return;
  }
  if (state.refreshFailures > 0)
    LOG_INFO << "Duress screen: guard answers again";
  state.refreshFailures = 0;
  if (state.encodedKey != *input.fetched || state.keyState == KeyState::Unknown)
    state.clean.clear();
  state.encodedKey = *input.fetched;
  state.keyDigest = input.fetched->empty() ? std::string{} : argus::hash::sha256Hex(*input.fetched);
  state.key = input.fetched->empty() ? std::nullopt : secret_phrase::decode(*input.fetched);
  if (!input.fetched->empty() && !state.key)
    LOG_WARN << "Duress screen: guard sent a phrase this build cannot read; nothing is screened";
  state.keyState = state.key ? KeyState::Present : KeyState::Absent;
  state.nextRefresh = input.now + input.timing.refresh;
}

struct SettleInput
{
  DuressScreenState& state;
  const DuressScreenTiming& timing;
  int64_t userId{0};
  DuressDelivery delivery{DuressDelivery::Retry};
};

std::optional<int64_t> settle(const SettleInput& input)
{
  DuressScreenState& state = input.state;
  const auto found = std::ranges::find_if(
      state.pending, [userId = input.userId](const DuressScreenState::Pending& entry) { return entry.userId == userId; });
  if (found == state.pending.end())
    return std::nullopt;
  if (input.delivery != DuressDelivery::Retry) {
    const int64_t row = found->row;
    state.pending.erase(found);
    if (input.delivery == DuressDelivery::Dropped)
      state.refreshRequested = true;
    return row;
  }
  ++found->failures;
  if (found->failures == 1)
    LOG_WARN << "Safety relay: guard did not take an alert yet; it is retried";
  found->nextAt = Clock::now() + backoff({.timing = input.timing, .failures = found->failures});
  return std::nullopt;
}
}

DuressScreen::DuressScreen(DuressScreenDependencies dependencies, DuressScreenTiming timing)
    : dependencies_(std::move(dependencies)), timing_(timing), state_(std::make_shared<DuressScreenState>())
{
}

DuressScreen::~DuressScreen()
{
  requestStop();
}

void DuressScreen::start()
{
  if (worker_.joinable() || !dependencies_.link)
    return;
  state_->finished.store(false, std::memory_order_release);
  worker_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

turn::ScreenedText DuressScreen::screen(const turn::ScreenedUtterance& utterance) const
{
  const int64_t heardAt = now();
  turn::ScreenedText kept{.text = std::string(utterance.text), .withheld = false};
  if (utterance.userId <= 0 || utterance.text.empty() || !dependencies_.eligible ||
      !dependencies_.eligible(utterance.role))
    return kept;
  const DuressScreenState::Fingerprint fingerprint = fingerprintOf(utterance.text);
  std::optional<secret_phrase::PhraseKey> key;
  std::string encoded;
  std::string digest;
  {
    const std::scoped_lock lock(state_->mutex);
    if (state_->stopped.load(std::memory_order_acquire) || state_->keyState == KeyState::Absent)
      return kept;
    if (state_->keyState == KeyState::Unknown) {
      if (state_->late.size() >= timing_.queueLimit)
        state_->late.pop_front();
      state_->late.push_back(
          {.userId = utterance.userId, .role = utterance.role, .text = kept.text, .heardAt = heardAt});
    }
    else {
      const auto memo = state_->clean.find(utterance.userId);
      if (memo != state_->clean.end() && memo->second.fingerprint == fingerprint &&
          Clock::now() - memo->second.at < timing_.memo)
        return kept;
      key = state_->key;
      encoded = state_->encodedKey;
      digest = state_->keyDigest;
    }
  }
  if (!key) {
    state_->wake.notify_one();
    return kept;
  }
  if (!secret_phrase::spokenIn(*key, utterance.text)) {
    const std::scoped_lock lock(state_->mutex);
    if (state_->encodedKey == encoded)
      state_->clean[utterance.userId] = {.fingerprint = fingerprint, .at = Clock::now()};
    return kept;
  }
  hold({.userId = utterance.userId, .role = utterance.role, .keyDigest = digest, .saidAt = heardAt});
  return {.text = std::string(llm_speech::kWithheldUtterance), .withheld = true};
}

void DuressScreen::hold(const DuressRaiseCreateInput& raise) const
{
  bool waitingAlready = false;
  {
    const std::scoped_lock lock(state_->mutex);
    waitingAlready = waiting(*state_, raise.userId);
  }
  std::optional<int64_t> row;
  if (!waitingAlready && dependencies_.outbox)
    row = dependencies_.outbox->add(raise);
  {
    const std::scoped_lock lock(state_->mutex);
    queueRaise(*state_,
               {.row = row.value_or(0),
                .userId = raise.userId,
                .role = raise.role,
                .keyDigest = raise.keyDigest,
                .saidAt = raise.saidAt,
                .failures = 0,
                .nextAt = Clock::now()});
  }
  state_->wake.notify_one();
}

void DuressScreen::forget(int64_t row) const
{
  if (dependencies_.outbox && row > 0)
    dependencies_.outbox->remove(row);
}

std::vector<DuressRaiseRow> DuressScreen::stored() const
{
  return dependencies_.outbox ? dependencies_.outbox->pending() : std::vector<DuressRaiseRow>{};
}

int64_t DuressScreen::now() const
{
  return dependencies_.clock ? dependencies_.clock() : epochSeconds();
}

void DuressScreen::requestRefresh() const
{
  {
    const std::scoped_lock lock(state_->mutex);
    state_->refreshRequested = true;
  }
  state_->wake.notify_one();
}

void DuressScreen::requestStop()
{
  state_->stopped.store(true, std::memory_order_release);
  if (worker_.joinable())
    worker_.request_stop();
}

bool DuressScreen::drained() const
{
  return state_->finished.load(std::memory_order_acquire);
}

bool DuressScreen::idle() const
{
  const std::scoped_lock lock(state_->mutex);
  return !state_->busy && !state_->refreshRequested && state_->pending.empty() &&
         (state_->late.empty() || state_->keyState == KeyState::Unknown);
}

void DuressScreen::run(const std::stop_token& stop) const
{
  DuressScreenState& state = *state_;
  const std::vector<DuressRaiseRow> carried = stored();
  std::unique_lock lock(state.mutex);
  if (!carried.empty()) {
    const int64_t oldest = std::ranges::min(carried, {}, &DuressRaiseRow::saidAt).saidAt;
    LOG_WARN << "Safety relay: " << carried.size() << " alert(s) written down before this start wait for guard, "
             << "the oldest for " << std::max<int64_t>(0, now() - oldest) << " s";
  }
  for (const DuressRaiseRow& row : carried)
    queueRaise(state,
               {.row = row.id,
                .userId = row.userId,
                .role = row.role,
                .keyDigest = row.keyDigest,
                .saidAt = row.saidAt,
                .failures = 0,
                .nextAt = Clock::now()});
  while (!stop.stop_requested()) {
    const Clock::time_point now = Clock::now();
    if (state.refreshRequested || now >= state.nextRefresh) {
      state.refreshRequested = false;
      state.busy = true;
      lock.unlock();
      const std::optional<std::string> fetched = dependencies_.link->phraseKey();
      lock.lock();
      state.busy = false;
      adoptKey({.state = state, .timing = timing_, .fetched = fetched, .now = now});
      continue;
    }
    if (state.keyState != KeyState::Unknown && !state.late.empty()) {
      DuressScreenState::Late item = std::move(state.late.front());
      state.late.pop_front();
      if (state.keyState != KeyState::Present || !state.key)
        continue;
      const secret_phrase::PhraseKey key = *state.key;
      const std::string digest = state.keyDigest;
      state.busy = true;
      lock.unlock();
      if (secret_phrase::spokenIn(key, item.text))
        hold({.userId = item.userId, .role = item.role, .keyDigest = digest, .saidAt = item.heardAt});
      lock.lock();
      state.busy = false;
      continue;
    }
    const auto due = std::ranges::find_if(
        state.pending, [now](const DuressScreenState::Pending& entry) { return entry.nextAt <= now; });
    if (state.keyState != KeyState::Unknown && due != state.pending.end()) {
      if (!sameKey(state, *due)) {
        const int64_t row = due->row;
        state.pending.erase(due);
        state.busy = true;
        lock.unlock();
        LOG_WARN << "Safety relay: an alert outlived the phrase it matched; it is dropped";
        forget(row);
        lock.lock();
        state.busy = false;
        continue;
      }
      const DuressRaise raise{
          .userId = due->userId, .role = due->role, .key = state.encodedKey, .saidAt = due->saidAt};
      state.busy = true;
      lock.unlock();
      const DuressDelivery delivery = dependencies_.link->raise(raise);
      lock.lock();
      const std::optional<int64_t> settled =
          settle({.state = state, .timing = timing_, .userId = raise.userId, .delivery = delivery});
      if (settled) {
        lock.unlock();
        forget(*settled);
        lock.lock();
      }
      state.busy = false;
      continue;
    }
    Clock::time_point wakeAt = state.nextRefresh;
    if (state.keyState != KeyState::Unknown)
      for (const auto& entry : state.pending)
        wakeAt = std::min(wakeAt, entry.nextAt);
    state.wake.wait_until(lock, stop, wakeAt, [&state] {
      return state.refreshRequested || raiseDue(state) ||
             (state.keyState != KeyState::Unknown && !state.late.empty());
    });
  }
  std::vector<DuressScreenState::Pending> left = std::move(state.pending);
  state.pending.clear();
  const KeyState keyState = state.keyState;
  const bool keyKnown = state.key.has_value();
  const std::string encoded = state.encodedKey;
  const std::string digest = state.keyDigest;
  lock.unlock();
  for (const auto& entry : left) {
    if (keyState == KeyState::Unknown)
      continue;
    if (keyState != KeyState::Present || !keyKnown || digest != entry.keyDigest) {
      forget(entry.row);
      continue;
    }
    const DuressDelivery delivery =
        dependencies_.link->raise({.userId = entry.userId, .role = entry.role, .key = encoded, .saidAt = entry.saidAt});
    if (delivery == DuressDelivery::Retry) {
      if (entry.row > 0)
        LOG_WARN << "Safety relay: an alert could not be handed to guard before the stop; it waits for the next start";
      else
        LOG_ERROR << "Safety relay: an alert could not be handed to guard before the stop";
      continue;
    }
    forget(entry.row);
  }
  state.finished.store(true, std::memory_order_release);
}
