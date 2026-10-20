#pragma once

#include <feature/call/services/call-preference-service.hxx>
#include <feature/conversation/services/conversation-ports.hxx>

#include <cstdint>

class CallLiveAnnounce final : public LiveAnnouncePreference
{
public:
  CallLiveAnnounce() = default;

  [[nodiscard]] drogon::Task<bool> allows(int64_t userId) const override;

private:
  CallPreferenceService service_;
};
