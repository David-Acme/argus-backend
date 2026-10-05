#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/services/face/face-check.hxx>
#include <shared/services/face/member-match.hxx>
#include <string>

struct FaceSignInResult
{
  FaceCheckStatus check{FaceCheckStatus::Unavailable};
  std::optional<MemberMatch> match;
};

class FaceSignInService
{
public:
  [[nodiscard]] drogon::Task<FaceSignInResult> identify(std::string image) const;

private:
  MemberMatcher matcher_;
};
