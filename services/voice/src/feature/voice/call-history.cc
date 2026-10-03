#include "call-history.hxx"

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace
{

constexpr size_t kEarlierLineChars = 140;

std::string_view langDisplayName(VoiceLang lang)
{
  return lang == VoiceLang::En ? "English" : "Spanish";
}

std::string trimmed(std::string_view text, std::string_view blanks)
{
  const auto first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(blanks);
  return std::string(text.substr(first, last - first + 1));
}

std::string cleaned(std::string_view text, bool keepNewlines)
{
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (keepNewlines && c == '\n')
      out.push_back(c);
    else
      out.push_back(byte < 0x20U ? ' ' : c);
  }
  return out;
}

bool isNoteKind(CallEntryKind kind)
{
  return kind == CallEntryKind::Note || kind == CallEntryKind::Situation;
}

}

std::string utf8Prefix(std::string_view text, size_t maxBytes)
{
  if (text.size() <= maxBytes)
    return std::string(text);
  size_t end = maxBytes;
  while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U)
    --end;
  return std::string(text.substr(0, end));
}

std::string sanitizedLine(std::string_view text, size_t limit)
{
  return trimmed(utf8Prefix(cleaned(text, false), limit), " ");
}

std::string sanitizedBlock(std::string_view text, size_t limit)
{
  return trimmed(utf8Prefix(cleaned(text, true), limit), " \n");
}

std::string callSystemPrompt(VoiceLang lang)
{
  std::string prompt =
      "You are Argus, a warm, natural home voice assistant for a local "
      "security camera system.\n"
      "Guidelines:\n"
      "- Reply strictly in ";
  prompt += langDisplayName(lang);
  prompt +=
      ". Never switch to another language.\n"
      "- Speak like a person, not a help desk: short, warm and direct.\n"
      "- Address the user informally (\"tú\" in Spanish), never \"usted\".\n"
      "- Never speak as if you were the user: the user's facts are yours to "
      "describe, not to own.\n"
      "- Engage with what the user just said; never answer with generic "
      "offers such as \"how can I help you\".\n"
      "- Answer in at most two short sentences and stop there.\n"
      "- End every sentence with a period, question mark or exclamation "
      "mark; split long ideas into several short sentences so the reply "
      "sounds like natural speech when spoken aloud.\n"
      "- Your reply is spoken aloud: natural sentences, no lists, no "
      "symbols or abbreviations.\n"
      "- If you do not know something, say so honestly; do not invent.\n"
      "- If you do not know the user's name, ask for it once, naturally.\n"
      "- System notes are facts from the app: the cameras, the guard mode, "
      "the agenda, camera events and what the app could or could not do. Use "
      "them to answer; they are never the user's words.\n";
  return prompt;
}

CallHistory::CallHistory(VoiceLang lang, CallHistoryLimits limits)
    : lang_(lang), limits_(limits)
{
  entries_.push_back({.kind = CallEntryKind::Prompt,
                      .message = {.role = "system", .content = callSystemPrompt(lang_)}});
}

void CallHistory::addNote(const std::string& note)
{
  if (note.empty() || std::ranges::find(notes_, note) != notes_.end())
    return;
  notes_.push_back(note);
  if (notes_.size() > limits_.maxNotes)
    notes_.erase(notes_.begin());
  if (!cached_) {
    rebuildPrompt();
    return;
  }
  entries_.push_back({.kind = CallEntryKind::Note, .message = {.role = "system", .content = note}});
}

void CallHistory::setSituation(const std::string& situation)
{
  if (situation == situation_)
    return;
  situation_ = situation;
  if (!cached_) {
    rebuildPrompt();
    return;
  }
  if (!situation_.empty())
    entries_.push_back(
        {.kind = CallEntryKind::Situation, .message = {.role = "system", .content = situation_}});
}

void CallHistory::addEvent(const std::string& event)
{
  if (event.empty())
    return;
  entries_.push_back({.kind = CallEntryKind::Event, .message = {.role = "system", .content = event}});
}

void CallHistory::addUser(const std::string& text)
{
  entries_.push_back({.kind = CallEntryKind::User, .message = {.role = "user", .content = text}});
}

void CallHistory::addTone(const std::string& tone)
{
  const std::string note = trimmed(tone, " \n");
  if (note.empty())
    return;
  entries_.push_back({.kind = CallEntryKind::Tone, .message = {.role = "system", .content = note}});
}

void CallHistory::addAssistant(const std::string& text)
{
  entries_.push_back({.kind = CallEntryKind::Assistant, .message = {.role = "assistant", .content = text}});
}

void CallHistory::rollbackUser()
{
  const auto turn = std::ranges::find_if(entries_ | std::views::reverse, [](const CallEntry& entry) {
    return entry.kind == CallEntryKind::User || entry.kind == CallEntryKind::Assistant;
  });
  if (turn == std::ranges::rend(entries_) || turn->kind != CallEntryKind::User)
    return;
  const auto from = std::prev(turn.base());
  const auto kept = std::remove_if(from, entries_.end(), [](const CallEntry& entry) {
    return entry.kind == CallEntryKind::User || entry.kind == CallEntryKind::Tone;
  });
  entries_.erase(kept, entries_.end());
}

size_t CallHistory::userTurns() const
{
  return static_cast<size_t>(std::ranges::count_if(
      entries_, [](const CallEntry& entry) { return entry.kind == CallEntryKind::User; }));
}

std::vector<ChatMessage> CallHistory::request()
{
  cached_ = true;
  std::vector<ChatMessage> messages;
  messages.reserve(entries_.size());
  for (const auto& entry : entries_)
    messages.push_back(entry.message);
  return messages;
}

void CallHistory::trim()
{
  if (userTurns() <= limits_.maxTurns)
    return;
  size_t seen = 0;
  size_t keepFrom = entries_.size();
  for (size_t i = entries_.size(); i-- > 1;) {
    if (entries_[i].kind != CallEntryKind::User)
      continue;
    keepFrom = i;
    if (++seen == limits_.keepTurns)
      break;
  }
  for (size_t i = 1; i < keepFrom; ++i)
    remember(entries_[i]);
  entries_.erase(entries_.begin() + 1, entries_.begin() + static_cast<std::ptrdiff_t>(keepFrom));
  std::erase_if(entries_, [](const CallEntry& entry) { return isNoteKind(entry.kind); });
  rebuildPrompt();
}

void CallHistory::remember(const CallEntry& entry)
{
  std::string_view label;
  switch (entry.kind) {
    case CallEntryKind::User:
      label = "User: ";
      break;
    case CallEntryKind::Assistant:
      label = "You: ";
      break;
    case CallEntryKind::Event:
      label = "App: ";
      break;
    default:
      return;
  }
  const std::string line = sanitizedLine(entry.message.content, kEarlierLineChars);
  if (line.empty())
    return;
  earlier_.push_back("- " + std::string(label) + line);
  if (earlier_.size() > limits_.maxEarlier)
    earlier_.erase(earlier_.begin(),
                   earlier_.begin() + static_cast<std::ptrdiff_t>(earlier_.size() - limits_.maxEarlier));
}

void CallHistory::rebuildPrompt()
{
  std::string prompt = callSystemPrompt(lang_);
  for (const auto& note : notes_)
    prompt += "\n" + note;
  if (!situation_.empty())
    prompt += "\n" + situation_;
  if (!earlier_.empty()) {
    prompt += "\nEarlier in this call, oldest first:";
    for (const auto& line : earlier_)
      prompt += "\n" + line;
  }
  entries_.front().message.content = std::move(prompt);
}
