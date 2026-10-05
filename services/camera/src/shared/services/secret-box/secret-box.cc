#include "secret-box.hxx"

#include <text/base64.hxx>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace
{
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

struct KeyHolder
{
  std::mutex mutex;
  std::optional<std::array<uint8_t, secret_box::kKeyBytes>> key;
};

KeyHolder& holder()
{
  static KeyHolder keys;
  return keys;
}

std::optional<std::array<uint8_t, secret_box::kKeyBytes>> currentKey()
{
  KeyHolder& keys = holder();
  std::scoped_lock lock(keys.mutex);
  return keys.key;
}

const unsigned char* bytesOf(std::string_view text)
{
  return reinterpret_cast<const unsigned char*>(text.data());
}

bool readAll(int fd, std::span<uint8_t> out)
{
  std::size_t done = 0;
  while (done < out.size()) {
    const ssize_t got = ::read(fd, out.data() + done, out.size() - done);
    if (got < 0 && errno == EINTR)
      continue;
    if (got <= 0)
      return false;
    done += static_cast<std::size_t>(got);
  }
  return ::read(fd, out.data(), 1) == 0;
}

bool writeAll(int fd, std::span<const uint8_t> in)
{
  std::size_t done = 0;
  while (done < in.size()) {
    const ssize_t put = ::write(fd, in.data() + done, in.size() - done);
    if (put < 0 && errno == EINTR)
      continue;
    if (put <= 0)
      return false;
    done += static_cast<std::size_t>(put);
  }
  return ::fsync(fd) == 0;
}
}

namespace secret_box
{
bool installKey(std::span<const uint8_t> key)
{
  if (key.size() != kKeyBytes)
    return false;
  std::array<uint8_t, kKeyBytes> copy{};
  std::ranges::copy(key, copy.begin());
  KeyHolder& keys = holder();
  std::scoped_lock lock(keys.mutex);
  keys.key = copy;
  return true;
}

void clearKey()
{
  KeyHolder& keys = holder();
  std::scoped_lock lock(keys.mutex);
  if (keys.key)
    OPENSSL_cleanse(keys.key->data(), keys.key->size());
  keys.key.reset();
}

bool hasKey()
{
  return currentKey().has_value();
}

KeyFileResult loadOrCreateKey(const std::string& path)
{
  std::array<uint8_t, kKeyBytes> key{};
  if (const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC); fd >= 0) {
    const bool read = readAll(fd, key);
    ::close(fd);
    const bool installed = read && installKey(key);
    OPENSSL_cleanse(key.data(), key.size());
    return installed ? KeyFileResult::Loaded : KeyFileResult::Failed;
  }
  if (errno != ENOENT)
    return KeyFileResult::Failed;
  if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1)
    return KeyFileResult::Failed;
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0)
    return KeyFileResult::Failed;
  const bool written = ::fchmod(fd, S_IRUSR | S_IWUSR) == 0 && writeAll(fd, key);
  ::close(fd);
  if (!written) {
    ::unlink(path.c_str());
    OPENSSL_cleanse(key.data(), key.size());
    return KeyFileResult::Failed;
  }
  const bool installed = installKey(key);
  OPENSSL_cleanse(key.data(), key.size());
  return installed ? KeyFileResult::Created : KeyFileResult::Failed;
}

bool isSealed(std::string_view stored)
{
  return stored.starts_with(kPrefix);
}

std::string seal(const SealInput& input)
{
  if (input.plain.empty())
    return {};
  const auto key = currentKey();
  if (!key)
    return std::string(input.plain);
  std::array<uint8_t, kNonceBytes> nonce{};
  if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
    return {};
  CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  std::vector<uint8_t> sealed(kNonceBytes + input.plain.size() + kTagBytes);
  std::ranges::copy(nonce, sealed.begin());
  int written = 0;
  int finished = 0;
  const bool ok =
      context &&
      EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceBytes),
                          nullptr) == 1 &&
      EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key->data(), nonce.data()) == 1 &&
      EVP_EncryptUpdate(context.get(), nullptr, &written, bytesOf(input.label),
                        static_cast<int>(input.label.size())) == 1 &&
      EVP_EncryptUpdate(context.get(), sealed.data() + kNonceBytes, &written,
                        bytesOf(input.plain), static_cast<int>(input.plain.size())) == 1 &&
      EVP_EncryptFinal_ex(context.get(), sealed.data() + kNonceBytes + written, &finished) == 1 &&
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagBytes),
                          sealed.data() + kNonceBytes + input.plain.size()) == 1;
  if (!ok)
    return {};
  return std::string(kPrefix) +
         base64::encode(std::string_view(reinterpret_cast<const char*>(sealed.data()), sealed.size()));
}

std::string open(const OpenInput& input)
{
  if (!isSealed(input.stored))
    return std::string(input.stored);
  const auto key = currentKey();
  if (!key)
    return {};
  const auto decoded = base64::decode(input.stored.substr(kPrefix.size()));
  if (!decoded || decoded->size() < kNonceBytes + kTagBytes)
    return {};
  const std::string_view sealed = *decoded;
  const std::string_view nonce = sealed.substr(0, kNonceBytes);
  const std::string_view cipher = sealed.substr(kNonceBytes, sealed.size() - kNonceBytes - kTagBytes);
  std::string tag(sealed.substr(sealed.size() - kTagBytes));
  CipherContext context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  std::string plain(cipher.size(), '\0');
  int written = 0;
  int finished = 0;
  const bool ok =
      context &&
      EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceBytes),
                          nullptr) == 1 &&
      EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key->data(), bytesOf(nonce)) == 1 &&
      EVP_DecryptUpdate(context.get(), nullptr, &written, bytesOf(input.label),
                        static_cast<int>(input.label.size())) == 1 &&
      EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(plain.data()), &written,
                        bytesOf(cipher), static_cast<int>(cipher.size())) == 1 &&
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagBytes),
                          tag.data()) == 1 &&
      EVP_DecryptFinal_ex(context.get(),
                          reinterpret_cast<unsigned char*>(plain.data()) + written, &finished) == 1;
  if (!ok) {
    OPENSSL_cleanse(plain.data(), plain.size());
    return {};
  }
  return plain;
}
}
