#pragma once

#include <shared/services/stream/upstream-http.hxx>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct CachedFragment
{
  std::shared_ptr<const std::string> bytes;
  upstream_http::FragmentKind kind{upstream_http::FragmentKind::Other};
  int64_t arrivedMs{0};
};

struct GopFreshness
{
  int64_t nowMs{0};
  int64_t maxAgeMs{0};
};

class GopCache
{
public:
  explicit GopCache(size_t capacityBytes) : capacityBytes_(capacityBytes) {}

  void add(const CachedFragment& fragment);
  void clear();
  [[nodiscard]] bool freshAt(const GopFreshness& freshness) const;

  [[nodiscard]] const std::vector<CachedFragment>& fragments() const
  {
    return fragments_;
  }
  [[nodiscard]] size_t bytes() const { return bytes_; }

private:
  size_t capacityBytes_{0};
  size_t bytes_{0};
  std::vector<CachedFragment> fragments_;
};
