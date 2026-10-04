#include "turn-transcript.hxx"

#include <algorithm>
#include <drogon/drogon.h>
#include <exception>
#include <utility>

TurnTranscript::TurnTranscript(TurnTranscriptInput input)
    : stt_(input.stt), input_(std::move(input.stream)), flushSilenceFrames_(std::max(1, input.flushSilenceFrames))
{
}

int TurnTranscript::flushFramesFor(int minSilenceFrames)
{
  return std::max(2, minSilenceFrames / 3);
}

void TurnTranscript::fail(const char* step)
{
  LOG_DEBUG << "Voice: streamed STT " << step << " failed; the turn falls back to one request";
  stream_.reset();
}

void TurnTranscript::follow(const VadService& vad)
{
  if (!vad.inSpeech()) {
    stream_.reset();
    pushed_ = 0;
    flushed_ = false;
    tried_ = false;
    return;
  }
  if (!stream_ && !tried_) {
    tried_ = true;
    try {
      stream_ = stt_.openStream(input_);
    }
    catch (const std::exception&) {
      fail("open");
    }
  }
  if (!stream_)
    return;

  const auto utterance = vad.utterance();
  const int silence = vad.silenceFrames();
  if (flushed_ && silence > 0)
    return;
  flushed_ = false;
  try {
    if (utterance.size() > pushed_) {
      stream_->push(utterance.subspan(pushed_));
      pushed_ = utterance.size();
    }
    if (silence >= flushSilenceFrames_) {
      stream_->flush();
      flushed_ = true;
      LOG_DEBUG << "Voice: STT stream flushed at " << pushed_ << " samples";
    }
  }
  catch (const std::exception&) {
    fail("push");
  }
}

std::optional<std::string> TurnTranscript::finish(std::span<const float> turn)
{
  if (!stream_)
    return std::nullopt;
  try {
    LOG_DEBUG << "Voice: STT stream finishes a turn of " << turn.size() << " samples, " << pushed_
              << " pushed, flushed " << flushed_;
    if (!flushed_ && turn.size() > pushed_)
      stream_->push(turn.subspan(pushed_));
    std::string text = stream_->finish();
    stream_.reset();
    if (text.empty())
      return std::nullopt;
    return text;
  }
  catch (const std::exception&) {
    fail("finish");
    return std::nullopt;
  }
}
