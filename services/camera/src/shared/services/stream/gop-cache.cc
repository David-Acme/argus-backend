#include "gop-cache.hxx"

void GopCache::add(const CachedFragment& fragment)
{
  if (capacityBytes_ == 0 || !fragment.bytes)
    return;
  if (fragment.kind == upstream_http::FragmentKind::VideoKey)
    clear();
  else if (fragments_.empty())
    return;
  if (bytes_ + fragment.bytes->size() > capacityBytes_) {
    clear();
    return;
  }
  bytes_ += fragment.bytes->size();
  fragments_.push_back(fragment);
}

void GopCache::clear()
{
  fragments_.clear();
  bytes_ = 0;
}
