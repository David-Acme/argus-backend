#pragma once

#include <feature/voice/speaker-verdict.hxx>
#include <llm/llm-service.hxx>
#include <voice/voice-lang.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class CallEntryKind : uint8_t
{
  Prompt,
  Note,
  Situation,
  Event,
  User,
  Tone,
  Assistant,
  Notice,
  Speaker
};

struct CallSpeakerNote
{
  VoiceSpeakerVerdict verdict{VoiceSpeakerVerdict::Unknown};
  std::string who;
  std::string holder;
};

struct CallEntry
{
  CallEntryKind kind{CallEntryKind::Prompt};
  ChatMessage message;
};

struct CallHistoryLimits
{
  size_t maxTurns{10};
  size_t keepTurns{5};
  size_t maxNotes{6};
  size_t maxEarlier{12};
};

class CallHistory
{
public:
  explicit CallHistory(VoiceLang lang, CallHistoryLimits limits = {});

  void addNote(const std::string& note);
  void setSituation(const std::string& situation);
  void addEvent(const std::string& event);
  void addUser(const std::string& text);
  void addTone(const std::string& tone);
  void addAssistant(const std::string& text);
  void addNotice(const std::string& spoken);
  void addSpeakerNote(const CallSpeakerNote& note);
  void rollbackUser();
  bool trim();

  [[nodiscard]] std::vector<ChatMessage> request();
  [[nodiscard]] const std::vector<CallEntry>& entries() const { return entries_; }
  [[nodiscard]] size_t size() const { return entries_.size(); }
  [[nodiscard]] size_t userTurns() const;

private:
  void rebuildPrompt();
  void remember(const CallEntry& entry);

  VoiceLang lang_;
  CallHistoryLimits limits_;
  std::vector<CallEntry> entries_;
  std::vector<std::string> notes_;
  std::string situation_;
  std::vector<std::string> earlier_;
  bool cached_{false};
};

[[nodiscard]] std::string callSystemPrompt(VoiceLang lang);
[[nodiscard]] std::string utf8Prefix(std::string_view text, size_t maxBytes);
[[nodiscard]] std::string sanitizedLine(std::string_view text, size_t limit);
[[nodiscard]] std::string sanitizedBlock(std::string_view text, size_t limit);
