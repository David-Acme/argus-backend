#include "call-live-announce.hxx"

drogon::Task<bool> CallLiveAnnounce::allows(int64_t userId) const
{
  const CallPreferenceSchema preference = co_await service_.read(userId);
  co_return preference.liveAnnounce;
}
