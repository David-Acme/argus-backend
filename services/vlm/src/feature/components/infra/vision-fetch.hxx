#pragma once

#include <http/download/chunk-transport.hxx>
#include <http/download/file-download.hxx>
#include <settings/component-host.hxx>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

struct VisionFetchInput
{
  std::shared_ptr<file_download::ChunkTransport> transport;
  file_download::DownloadPolicy policy;
};

struct FetchFailureInput
{
  file_download::DownloadFailure failure{file_download::DownloadFailure::None};
  std::filesystem::path target;
  std::int64_t remainingBytes{0};
};

[[nodiscard]] std::string fetchFailureReason(const FetchFailureInput& input);

[[nodiscard]] ComponentFetch visionFetch(VisionFetchInput input);
