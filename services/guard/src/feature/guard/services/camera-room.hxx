#pragma once

#include <feature/guard/repositories/camera-context/camera-context-query.hxx>

#include <string>

[[nodiscard]] std::string cameraRoomKey(const GuardCameraContext& context);
