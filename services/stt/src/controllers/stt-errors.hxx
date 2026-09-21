#pragma once

#include <errors/error-code.hxx>
#include <errors/error-definition.hxx>

// What the speech-to-text API refuses with: the engine's own status and the
// two shapes of a body that is not the PCM stream the endpoint accepts.
namespace SttErrors
{
inline constexpr ErrorDefinition SpeechEngineNotLoaded{
    .code = ErrorCode::SttNotLoaded,
    .status = 503,
    .message = "Speech-to-text engine is not loaded"};
inline constexpr ErrorDefinition BodyNotPcmS16{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "Body must be audio/x-argus-pcm-s16"};
inline constexpr ErrorDefinition PcmBodyMisaligned{
    .code = ErrorCode::BadRequest,
    .status = 400,
    .message = "PCM body is not int16-aligned"};
} // namespace SttErrors
