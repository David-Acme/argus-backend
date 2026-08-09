#include "camera-audio-source.hxx"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

int fragmentTrackId(const std::string& fragment)
{
  if (fragment.size() < 8 || std::memcmp(fragment.data() + 4, "moof", 4) != 0)
    return -1;
  const uint32_t moofSize = be32at(fragment, 0);
  if (moofSize < 8 || moofSize > fragment.size())
    return -1;
  size_t offset = 8;
  while (offset + 8 <= moofSize) {
    const uint32_t boxSize = be32at(fragment, offset);
    if (boxSize < 8 || offset + boxSize > moofSize)
      return -1;
    if (std::memcmp(fragment.data() + offset + 4, "traf", 4) == 0) {
      const size_t trafEnd = offset + boxSize;
      size_t inner = offset + 8;
      while (inner + 8 <= trafEnd) {
        const uint32_t innerSize = be32at(fragment, inner);
        if (innerSize < 8 || inner + innerSize > trafEnd)
          return -1;
        if (std::memcmp(fragment.data() + inner + 4, "tfhd", 4) == 0 &&
            inner + 16 <= trafEnd)
          return static_cast<int>(be32at(fragment, inner + 12));
        inner += innerSize;
      }
      return -1;
    }
    offset += boxSize;
  }
  return -1;
}

bool audioInfoFromInit(const std::string& init, std::string& fourcc,
                       int& sampleRate, int& trackId)
{
  const size_t moov = findBox(init, "moov", 0, init.size());
  if (moov == std::string::npos)
    return false;
  const size_t moovEnd = moov + be32at(init, moov);

  size_t searchFrom = moov + 8;
  for (int attempt = 0; attempt < 8; ++attempt) {
    const size_t trak = findBox(init, "trak", searchFrom, moovEnd);
    if (trak == std::string::npos)
      return fourcc.empty();
    const size_t trakEnd = trak + be32at(init, trak);
    searchFrom = trakEnd;

    std::string candidate;
    int candidateRate = 0;
    int candidateTrack = -1;

    const size_t tkhd = findBox(init, "tkhd", trak + 8, trakEnd);
    if (tkhd != std::string::npos && tkhd + 8 + 20 <= trakEnd) {
      const uint32_t version = be32at(init, tkhd + 8) >> 24;
      const size_t trackIdPos = tkhd + 8 + 4 + (version == 1 ? 16 : 8);
      if (trackIdPos + 4 <= trakEnd)
        candidateTrack = static_cast<int>(be32at(init, trackIdPos));
    }

    const size_t mdia = findBox(init, "mdia", trak + 8, trakEnd);
    if (mdia == std::string::npos)
      continue;
    const size_t mdiaEnd = mdia + be32at(init, mdia);

    const size_t mdhd = findBox(init, "mdhd", mdia + 8, mdiaEnd);
    if (mdhd != std::string::npos && mdhd + 8 + 20 <= mdiaEnd) {
      const uint32_t version = be32at(init, mdhd + 8) >> 24;
      const size_t timescalePos = mdhd + 8 + 4 + (version == 1 ? 16 : 8);
      if (timescalePos + 4 <= mdiaEnd)
        candidateRate = static_cast<int>(be32at(init, timescalePos));
    }

    const size_t minf = findBox(init, "minf", mdia + 8, mdiaEnd);
    if (minf == std::string::npos)
      continue;
    const size_t minfEnd = minf + be32at(init, minf);
    const size_t stbl = findBox(init, "stbl", minf + 8, minfEnd);
    if (stbl == std::string::npos)
      continue;
    const size_t stblEnd = stbl + be32at(init, stbl);
    const size_t stsd = findBox(init, "stsd", stbl + 8, stblEnd);
    if (stsd == std::string::npos || stsd + 8 + 12 > stblEnd)
      continue;
    candidate = init.substr(stsd + 8 + 12, 4);

    if (candidate == "avc1" || candidate == "h264" || candidate == "hev1" ||
        candidate == "hvc1" || candidate == "mp4v" || candidate == "vp08" ||
        candidate == "vp09")
      continue;
    fourcc = candidate;
    sampleRate = candidateRate;
    trackId = candidateTrack;
    return true;
  }
  return fourcc.empty();
}

std::vector<int16_t> decodePcm(const std::string& fourcc,
                               const std::vector<uint8_t>& bytes, bool& ok)
{
  ok = true;
  if (fourcc == "alaw" || fourcc == "PCMA")
    return tapo_audio::decodeALaw(bytes);
  if (fourcc == "ulaw" || fourcc == "PCMU")
    return tapo_audio::decodeULaw(bytes);
  if (fourcc == "PCM" || fourcc == "ipcm" || fourcc == "in16" ||
      fourcc == "lpcm" || fourcc == "twos") {
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
  if (fourcc == "PCML" || fourcc == "sowt") {
    std::vector<int16_t> out;
    out.reserve(bytes.size() / 2);
    for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
      const int16_t sample = static_cast<int16_t>(
          static_cast<uint16_t>(bytes[i]) |
          (static_cast<uint16_t>(bytes[i + 1]) << 8));
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
  int audioTrackId{-1};
  std::string lastError;
  std::unique_ptr<AudioResampler> resampler;
  int64_t fragments{0};
  int64_t samplesDecoded{0};
  int64_t timeouts{0};
  std::chrono::steady_clock::time_point lastReport{};
  float lastRms{0.0F};
  FILE* dump{nullptr};
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
  const std::string path = "/api/stream.mp4?src=" + src + "&mp4=flac";
  upstream_http::Upstream up = upstream_http::open(host, port, path, 5);
  if (!up.ok) {
    const auto crlf = up.headers.find("\r\n");
    const std::string statusLine =
        up.headers.substr(0, crlf == std::string::npos ? up.headers.size() : crlf);
    impl_->lastError =
        "go2rtc: " + statusLine + " " + up.leftover.substr(0, 300);
    return false;
  }
  impl_->lastReport = std::chrono::steady_clock::now();
  if (const char* dumpPath = std::getenv("ARGUS_MIC_DUMP"))
    impl_->dump = std::fopen(dumpPath, "wb");

  impl_->fd = up.fd;
  impl_->reader = upstream_http::Fmp4Reader(
      {.chunked = upstream_http::isChunked(up.headers)});
  impl_->reader.onInit = [this](std::string init) {
    audioInfoFromInit(init, impl_->fourcc, impl_->sourceRate,
                      impl_->audioTrackId);
    if (impl_->sourceRate <= 0)
      impl_->sourceRate = 8000;
    impl_->resampler = std::make_unique<AudioResampler>(
        AudioResamplerInput{.sourceRate = impl_->sourceRate,
                            .targetRate = impl_->targetRate});
    std::cout << "[mic] init: fourcc=" << impl_->fourcc
              << " rate=" << impl_->sourceRate
              << " track=" << impl_->audioTrackId << "\n";
    if (impl_->dump) {
      std::fwrite(init.data(), 1, init.size(), impl_->dump);
      std::fflush(impl_->dump);
    }
  };
  impl_->reader.onFragment = [this](std::string fragment, bool) {
    impl_->fragments++;
    if (!impl_->resampler)
      return;
    const int track = fragmentTrackId(fragment);
    if (track < 0 || track != impl_->audioTrackId)
      return;
    if (impl_->dump) {
      std::fwrite(fragment.data(), 1, fragment.size(), impl_->dump);
      std::fflush(impl_->dump);
    }
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
    if (!out.empty()) {
      impl_->ring.push(out.data(), out.size());
      double sum = 0.0;
      for (const float v : out)
        sum += static_cast<double>(v) * v;
      impl_->lastRms =
          static_cast<float>(std::sqrt(sum / static_cast<double>(out.size())));
    }
    impl_->samplesDecoded += static_cast<int64_t>(pcm.size());
  };
  if (!up.leftover.empty())
    impl_->reader.feed(up.leftover.data(), up.leftover.size());
  return true;
}

bool CameraAudioSource::read(std::vector<float>& out)
{
  out.clear();
  const auto now = std::chrono::steady_clock::now();
  if (now - impl_->lastReport >= std::chrono::seconds(2)) {
    impl_->lastReport = now;
    std::cout << "[mic] frags=" << impl_->fragments
              << " decoded=" << impl_->samplesDecoded
              << " timeouts=" << impl_->timeouts
              << " ring=" << impl_->ring.size() << " rms=" << impl_->lastRms
              << "\n";
    impl_->fragments = 0;
    impl_->samplesDecoded = 0;
  }
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
    impl_->timeouts++;
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
  impl_->audioTrackId = -1;
  impl_->ring.clear();
  if (impl_->dump) {
    std::fclose(impl_->dump);
    impl_->dump = nullptr;
  }
}

bool CameraAudioSource::isOpen() const
{
  return impl_->fd >= 0;
}

const std::string& CameraAudioSource::lastError() const
{
  return impl_->lastError;
}
