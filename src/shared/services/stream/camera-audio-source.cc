#include "camera-audio-source.hxx"

#include <algorithm>
#include <cstring>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/upstream-http.hxx>
#include <shared/services/tapo/tapo-audio.hxx>
#include <shared/wrapper/audio/audio-resampler.hxx>
#include <shared/wrapper/audio/sample-ring.hxx>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace
{

constexpr int kRecvTimeoutSec = 1;

uint32_t be32at(const std::string& s, size_t offset)
{
  return (static_cast<uint32_t>(static_cast<unsigned char>(s[offset])) << 24) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[offset + 1]))
          << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(s[offset + 2]))
          << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(s[offset + 3]));
}

size_t findBox(const std::string& s, const char* type, size_t begin, size_t end)
{
  size_t offset = begin;
  while (offset + 8 <= end) {
    const uint32_t size = be32at(s, offset);
    if (size < 8 || offset + size > end)
      return std::string::npos;
    if (std::memcmp(s.data() + offset + 4, type, 4) == 0)
      return offset;
    offset += size;
  }
  return std::string::npos;
}

bool audioInfoFromInit(const std::string& init, std::string& fourcc,
                       int& sampleRate)
{
  const size_t moov = findBox(init, "moov", 0, init.size());
  if (moov == std::string::npos)
    return false;
  const size_t moovEnd = moov + be32at(init, moov);

  const size_t trak = findBox(init, "trak", moov + 8, moovEnd);
  if (trak == std::string::npos)
    return false;
  const size_t trakEnd = trak + be32at(init, trak);

  const size_t mdia = findBox(init, "mdia", trak + 8, trakEnd);
  if (mdia == std::string::npos)
    return false;
  const size_t mdiaEnd = mdia + be32at(init, mdia);

  const size_t mdhd = findBox(init, "mdhd", mdia + 8, mdiaEnd);
  if (mdhd != std::string::npos && mdhd + 8 + 20 <= mdiaEnd) {
    const uint32_t version = be32at(init, mdhd + 8) >> 24;
    const size_t timescalePos = mdhd + 8 + 4 + (version == 1 ? 16 : 8);
    if (timescalePos + 4 <= mdiaEnd)
      sampleRate = static_cast<int>(be32at(init, timescalePos));
  }

  const size_t minf = findBox(init, "minf", mdia + 8, mdiaEnd);
  if (minf == std::string::npos)
    return fourcc.empty();
  const size_t minfEnd = minf + be32at(init, minf);
  const size_t stbl = findBox(init, "stbl", minf + 8, minfEnd);
  if (stbl == std::string::npos)
    return fourcc.empty();
  const size_t stblEnd = stbl + be32at(init, stbl);
  const size_t stsd = findBox(init, "stsd", stbl + 8, stblEnd);
  if (stsd == std::string::npos || stsd + 8 + 12 > stblEnd)
    return fourcc.empty();
  fourcc = init.substr(stsd + 8 + 12, 4);
  return true;
}

std::vector<int16_t> decodePcm(const std::string& fourcc,
                               const std::vector<uint8_t>& bytes, bool& ok)
{
  ok = true;
  if (fourcc == "alaw")
    return tapo_audio::decodeALaw(bytes);
  if (fourcc == "ulaw")
    return tapo_audio::decodeULaw(bytes);
  if (fourcc == "ipcm" || fourcc == "in16" || fourcc == "lpcm" ||
      fourcc == "twos" || fourcc == "sowt") {
    std::vector<int16_t> out;
    out.reserve(bytes.size() / 2);
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
      const int16_t sample =
          static_cast<int16_t>((static_cast<uint16_t>(bytes[i]) << 8) |
                               static_cast<uint16_t>(bytes[i + 1]));
      out.push_back(sample);
    }
    return out;
  }
  ok = false;
  return {};
}

} // namespace

struct CameraAudioSource::Impl
{
  explicit Impl(const CameraAudioInput& input)
      : reader(upstream_http::Fmp4ReaderInput{.chunked = false}),
        ring(input.ringCapacity)
  {
    cameraId = input.cameraId;
    targetRate = input.targetRate;
  }

  upstream_http::Fmp4Reader reader;
  SampleRing ring;
  std::string fourcc;
  int sourceRate{8000};
  int targetRate{16000};
  int64_t cameraId{1};
  int fd{-1};
  std::string lastError;
  std::unique_ptr<AudioResampler> resampler;
};

CameraAudioSource::CameraAudioSource(const CameraAudioInput& input)
    : impl_(new Impl(input))
{
}

CameraAudioSource::~CameraAudioSource()
{
  close();
}

bool CameraAudioSource::open()
{
  close();
  impl_->lastError.clear();

  const auto [host, port] =
      upstream_http::splitHostPort(Go2rtcManager::apiBase().substr(7));
  const std::string src = Go2rtcManager::streamName(impl_->cameraId);
  std::string path = "/api/stream.mp4?src=" + src + "&video=none&audio=pcma";
  upstream_http::Upstream up = upstream_http::open(host, port, path, 5);
  if (!up.ok) {
    path = "/api/stream.mp4?src=" + src + "&video=none";
    up = upstream_http::open(host, port, path, 5);
  }
  if (!up.ok) {
    impl_->lastError = "go2rtc audio upstream failed";
    return false;
  }

  impl_->fd = up.fd;
  impl_->reader = upstream_http::Fmp4Reader(
      {.chunked = upstream_http::isChunked(up.headers)});
  impl_->reader.onInit = [this](std::string init) {
    audioInfoFromInit(init, impl_->fourcc, impl_->sourceRate);
    if (impl_->sourceRate <= 0)
      impl_->sourceRate = 8000;
    impl_->resampler = std::make_unique<AudioResampler>(
        AudioResamplerInput{.sourceRate = impl_->sourceRate,
                            .targetRate = impl_->targetRate});
  };
  impl_->reader.onFragment = [this](std::string fragment, bool) {
    if (!impl_->resampler)
      return;
    const size_t mdat = findBox(fragment, "mdat", 0, fragment.size());
    if (mdat == std::string::npos)
      return;
    const uint32_t size = be32at(fragment, mdat);
    if (size <= 8)
      return;
    std::vector<uint8_t> bytes(fragment.begin() + static_cast<long>(mdat + 8),
                               fragment.begin() +
                                   static_cast<long>(mdat + size));
    bool ok = false;
    std::vector<int16_t> pcm = decodePcm(impl_->fourcc, bytes, ok);
    if (!ok) {
      impl_->lastError = "unsupported audio codec: " + impl_->fourcc;
      return;
    }
    std::vector<int16_t> up;
    impl_->resampler->process(pcm.data(), pcm.size(), up);
    std::vector<float> out;
    out.reserve(up.size());
    for (const auto sample : up)
      out.push_back(static_cast<float>(sample) / 32768.0F);
    if (!out.empty())
      impl_->ring.push(out.data(), out.size());
  };
  if (!up.leftover.empty())
    impl_->reader.feed(up.leftover.data(), up.leftover.size());
  return true;
}

bool CameraAudioSource::read(std::vector<float>& out)
{
  out.clear();
  if (impl_->fd < 0)
    return false;

  char buf[65536];
  for (;;) {
    const ssize_t n = ::recv(impl_->fd, buf, sizeof(buf), 0);
    if (n > 0) {
      impl_->reader.feed(buf, static_cast<size_t>(n));
      if (!impl_->lastError.empty()) {
        close();
        return false;
      }
      break;
    }
    if (n == 0) {
      close();
      return false;
    }
    if (errno == EINTR)
      continue;
    close();
    return false;
  }

  std::vector<float> tmp;
  tmp.resize(4096);
  while (impl_->ring.size() > 0) {
    const size_t take = std::min<size_t>(tmp.size(), impl_->ring.size());
    if (!impl_->ring.pop(tmp.data(), take))
      break;
    out.insert(out.end(), tmp.begin(), tmp.begin() + static_cast<long>(take));
  }
  return true;
}

void CameraAudioSource::close()
{
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->reader.reset();
  impl_->resampler.reset();
  impl_->fourcc.clear();
  impl_->ring.clear();
}

bool CameraAudioSource::isOpen() const
{
  return impl_->fd >= 0;
}

const std::string& CameraAudioSource::lastError() const
{
  return impl_->lastError;
}
