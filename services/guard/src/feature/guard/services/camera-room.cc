#include "camera-room.hxx"

std::string cameraRoomKey(const GuardCameraContext& context)
{
  const std::string environment = std::to_string(context.environmentId);
  switch (context.role) {
    case CameraRole::Living:
    case CameraRole::Kitchen:
    case CameraRole::Office:
    case CameraRole::Garage:
    case CameraRole::Register:
    case CameraRole::Storage:
    case CameraRole::Entrance:
      if (!context.outdoor)
        return environment + ":" + cameraRoleToString(context.role);
      break;
    case CameraRole::Other:
    case CameraRole::Perimeter:
    case CameraRole::PublicArea:
      break;
  }
  return environment + ":camera:" + std::to_string(context.cameraId);
}
