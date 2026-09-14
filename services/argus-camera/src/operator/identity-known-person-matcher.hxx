#pragma once

#include <operator/known-person-matcher.hxx>
#include <operator/operator-config.hxx>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

class IdentityClient;

// Recognizes and enrolls person crops through argus.identity (fleet-gated RPC).
// Best-shot selection is per (camera, track): a cached verdict never leaks
// between people and only a clearly better frame re-identifies.
class IdentityKnownPersonMatcher final : public IKnownPersonMatcher
{
public:
  explicit IdentityKnownPersonMatcher(IdentityConfig config);
  ~IdentityKnownPersonMatcher() override;

  std::optional<PersonMatch> match(const PersonCrop& crop) const override;

private:
  struct CacheEntry
  {
    int64_t firstMs{0};
    int64_t lastScanMs{0};
    double score{0.0};
    bool scanned{false};
    std::optional<PersonMatch> result;
  };

  std::string encodeCrop(const PersonCrop& crop) const;
  double cropQuality(const PersonCrop& crop) const;
  bool canEnroll(int64_t cameraId, int64_t stamp) const;

  IdentityConfig config_;
  std::unique_ptr<IdentityClient> client_;
  mutable std::mutex mutex_;
  mutable std::map<std::pair<int64_t, int64_t>, CacheEntry> cache_;
  mutable std::map<int64_t, int64_t> lastEnrollMs_;
};
