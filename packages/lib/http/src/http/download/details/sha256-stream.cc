#include "sha256-stream.hxx"

#include <array>
#include <openssl/evp.h>
#include <stdexcept>

namespace file_download::details
{

void Sha256Stream::ContextDeleter::operator()(evp_md_ctx_st* context) const
{
  EVP_MD_CTX_free(context);
}

Sha256Stream::Sha256Stream() : context_(EVP_MD_CTX_new())
{
  if (!context_)
    throw std::runtime_error("sha256: no digest context");
  reset();
}

void Sha256Stream::reset()
{
  if (EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) != 1)
    throw std::runtime_error("sha256: digest init failed");
}

void Sha256Stream::update(std::span<const char> bytes)
{
  if (bytes.empty())
    return;
  if (EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1)
    throw std::runtime_error("sha256: digest update failed");
}

std::string Sha256Stream::hexDigest()
{
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int length = 0;
  if (EVP_DigestFinal_ex(context_.get(), digest.data(), &length) != 1)
    throw std::runtime_error("sha256: digest final failed");
  constexpr std::string_view kHex = "0123456789abcdef";
  std::string hex;
  hex.reserve(static_cast<std::size_t>(length) * 2);
  for (const auto byte : std::span(digest).first(length))
  {
    hex.push_back(kHex[byte >> 4U]);
    hex.push_back(kHex[byte & 0x0FU]);
  }
  reset();
  return hex;
}

}
