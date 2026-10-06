#pragma once

#include <memory>
#include <span>
#include <string>

struct evp_md_ctx_st;

namespace file_download::details
{

class Sha256Stream
{
public:
  Sha256Stream();

  void reset();
  void update(std::span<const char> bytes);
  [[nodiscard]] std::string hexDigest();

private:
  struct ContextDeleter
  {
    void operator()(evp_md_ctx_st* context) const;
  };

  std::unique_ptr<evp_md_ctx_st, ContextDeleter> context_;
};

}
