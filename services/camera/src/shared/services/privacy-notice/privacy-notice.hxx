#pragma once

#include <config/camera-config.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <memory>
#include <string>
#include <string_view>

struct PrivacyDoubtNotice
{
  int64_t cameraId{0};
  std::string cameraName;
  int64_t atMs{0};
};

struct PrivacyNoticeText
{
  std::string title;
  std::string body;
};

class ICameraPrivacyNotifier
{
public:
  virtual ~ICameraPrivacyNotifier() = default;

  [[nodiscard]] virtual drogon::Task<bool> doubtful(
      const PrivacyDoubtNotice& notice) const = 0;
};

namespace privacy_notice
{

[[nodiscard]] PrivacyNoticeText text(const PrivacyDoubtNotice& notice,
                                     std::string_view lang);

void setNotifier(const ICameraPrivacyNotifier* notifier);
[[nodiscard]] const ICameraPrivacyNotifier* getNotifier();

class NotificationPrivacyNotifier : public ICameraPrivacyNotifier
{
public:
  explicit NotificationPrivacyNotifier(const PrivacyNoticeConfig& config);
  ~NotificationPrivacyNotifier() override;

  NotificationPrivacyNotifier(const NotificationPrivacyNotifier&) = delete;
  NotificationPrivacyNotifier& operator=(const NotificationPrivacyNotifier&) =
      delete;

  [[nodiscard]] drogon::Task<bool> doubtful(
      const PrivacyDoubtNotice& notice) const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
