#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <optional>
#include <string>

struct MessageAnnouncement
{
  int64_t userId{0};
  std::string text;
};

class MessageAnnouncer
{
public:
  MessageAnnouncer() = default;
  virtual ~MessageAnnouncer() = default;
  MessageAnnouncer(const MessageAnnouncer&) = delete;
  MessageAnnouncer& operator=(const MessageAnnouncer&) = delete;

  [[nodiscard]] virtual std::optional<bool> announce(const MessageAnnouncement& announcement) const = 0;
};

class LiveAnnouncePreference
{
public:
  LiveAnnouncePreference() = default;
  virtual ~LiveAnnouncePreference() = default;
  LiveAnnouncePreference(const LiveAnnouncePreference&) = delete;
  LiveAnnouncePreference& operator=(const LiveAnnouncePreference&) = delete;

  [[nodiscard]] virtual drogon::Task<bool> allows(int64_t userId) const = 0;
};

struct MessageNotice
{
  int64_t userId{0};
  std::string title;
  std::string body;
  Json::Value data{Json::objectValue};
  std::string commandId;
};

class MessageNoticeWriter
{
public:
  MessageNoticeWriter() = default;
  virtual ~MessageNoticeWriter() = default;
  MessageNoticeWriter(const MessageNoticeWriter&) = delete;
  MessageNoticeWriter& operator=(const MessageNoticeWriter&) = delete;

  [[nodiscard]] virtual drogon::Task<bool> notify(const MessageNotice& notice) const = 0;
};
