#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <string>

struct S3StoredObject
{
  std::string objectKey;
  std::string sha256;
  int64_t byteSize{0};
};

class S3StorageService
{
public:
  [[nodiscard]] bool isConfigured() const;
  drogon::Task<S3StoredObject> putPortrait(int64_t userId,
                                            const std::string& image) const;
  drogon::Task<std::string> get(const std::string& objectKey) const;
  drogon::Task<void> remove(const std::string& objectKey) const;
};
