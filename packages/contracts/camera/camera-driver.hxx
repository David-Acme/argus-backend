#pragma once

#include <cstdint>
#include <string>

// Which integration drives a camera; the control layer picks a driver from it.
enum class CameraDriver : uint8_t
{
  Tapo = 0,
  Onvif,
  Rtsp
};

inline std::string cameraDriverToString(CameraDriver d)
{
  switch (d) {
    case CameraDriver::Onvif:
      return "onvif";
    case CameraDriver::Rtsp:
      return "rtsp";
    case CameraDriver::Tapo:
      return "tapo";
  }
  return "tapo";
}

inline CameraDriver cameraDriverFromString(const std::string& s)
{
  if (s == "onvif")
    return CameraDriver::Onvif;
  if (s == "rtsp")
    return CameraDriver::Rtsp;
  return CameraDriver::Tapo;
}
