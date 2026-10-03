#pragma once

#include <cstdint>

enum class VoiceprintOutcome : uint8_t
{
  Ok = 0,
  Unavailable,
  NotEnrolled,
  Stale,
  SampleInvalid,
  SampleTooShort,
  SampleTooNoisy,
  SampleClipped,
  SamplesInconsistent,
  SampleCountInvalid,
  AlreadyEnrolled,
  VoiceTaken,
  ChallengeInvalid,
  ConsentRequired,
  Forbidden,
  FaceNotVerified,
  UserNotFound
};
