#pragma once

#include <drogon/utils/coroutine.h>

#include <optional>
#include <string>

struct VlmDescribeInput
{
  std::string jpeg;
  std::string prompt;
  std::string cameraId;
};

struct VlmDescribeResult
{
  std::string caption;
};

// Client for the argus-vlm internal wire (POST /vlm/v1/describe).
class VlmClient
{
public:
  explicit VlmClient(std::string baseUrl, double timeoutS = 8.0);

  drogon::Task<std::optional<VlmDescribeResult>>
  describe(const VlmDescribeInput& input) const;

private:
  std::string baseUrl_;
  double timeoutS_;
};
