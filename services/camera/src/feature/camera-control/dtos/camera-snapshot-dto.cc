#include "camera-snapshot-dto.hxx"

#include <validation/validation_dsl.hxx>

CameraSnapshotDto CameraSnapshotDto::fromRequest(const drogon::HttpRequestPtr& request)
{
  CameraSnapshotDto dto;
  if (const std::string quality = request->getParameter("quality"); !quality.empty())
    dto.quality = quality;

  START_VALIDATION(CameraSnapshotDto, dto)
  IS_IN_OPTIONAL(quality, "main", "sub")
  END_VALIDATION()
  return dto;
}

CameraStream CameraSnapshotDto::stream() const
{
  const auto chosen = quality ? cameraStreamFromString(*quality) : std::nullopt;
  return chosen.value_or(camera_stream_role::streamFor(CameraStreamRole::Analysis));
}
