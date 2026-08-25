#include "camera-audio.hxx"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/audio/audio-resampler.hxx>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace
{
constexpr int kOutRate = 16000;
constexpr size_t kBlock8k = 1024;
} // namespace

struct CameraMic::Impl
{
  AVFormatContext* fmt{nullptr};
  int audioIndex{-1};
  AVCodecContext* dec{nullptr};
  AVPacket* pkt{nullptr};
  AVFrame* frame{nullptr};
  OnAudio onAudio;
  std::vector<int16_t> pending8k;
  int sourceRate{8000};
  std::unique_ptr<AudioResampler> resampler;
  std::string lastError;
  float smoothGain{1.0F};
  std::chrono::steady_clock::time_point lastAudioAt{};
  std::chrono::steady_clock::time_point deadline{};
  std::chrono::milliseconds recoverTimeout{3000};
};

int CameraMic::recoverCb(void* opaque)
{
  auto* impl = static_cast<Impl*>(opaque);
  return std::chrono::steady_clock::now() > impl->deadline ? 1 : 0;
}

CameraMic::CameraMic() = default;

CameraMic::~CameraMic() = default;

const std::string& CameraMic::lastError() const
{
  static const std::string empty;
  return impl_ ? impl_->lastError : empty;
}

bool CameraMic::open(const std::string& rtspUrl, OnAudio onAudio)
{
  close();
  impl_ = std::make_unique<Impl>();
  impl_->onAudio = std::move(onAudio);

  const int recoverMs =
      ConfigService::getInt("labs.voice_test.mic_recover_timeout_ms");
  impl_->recoverTimeout =
      std::chrono::milliseconds(recoverMs > 0 ? recoverMs : 3000);
  impl_->lastAudioAt = std::chrono::steady_clock::now();
  impl_->deadline = impl_->lastAudioAt + impl_->recoverTimeout;

  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "rw_timeout", "2000000", 0);
  av_dict_set(&opts, "rtsp_transport", "tcp", 0);
  impl_->fmt = avformat_alloc_context();
  impl_->fmt->interrupt_callback = {&CameraMic::recoverCb, impl_.get()};
  if (avformat_open_input(&impl_->fmt, rtspUrl.c_str(), nullptr, &opts) != 0) {
    av_dict_free(&opts);
    impl_.reset();
    return false;
  }
  av_dict_free(&opts);
  if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) {
    close();
    return false;
  }
  impl_->audioIndex =
      av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (impl_->audioIndex < 0) {
    close();
    return false;
  }
  impl_->sourceRate =
      impl_->fmt->streams[impl_->audioIndex]->codecpar->sample_rate;
  if (impl_->sourceRate <= 0)
    impl_->sourceRate = 8000;
  impl_->resampler = std::make_unique<AudioResampler>(
      AudioResamplerInput{.sourceRate = impl_->sourceRate,
                          .targetRate = kOutRate});

  const AVCodec* codec = avcodec_find_decoder(
      impl_->fmt->streams[impl_->audioIndex]->codecpar->codec_id);
  impl_->dec = avcodec_alloc_context3(codec);
  if (!impl_->dec ||
      avcodec_parameters_to_context(impl_->dec,
                                    impl_->fmt->streams[impl_->audioIndex]
                                        ->codecpar) < 0 ||
      avcodec_open2(impl_->dec, codec, nullptr) < 0) {
    close();
    return false;
  }
  impl_->pkt = av_packet_alloc();
  impl_->frame = av_frame_alloc();
  return true;
}

void CameraMic::close()
{
  if (!impl_)
    return;
  if (impl_->fmt)
    avformat_close_input(&impl_->fmt);
  if (impl_->dec)
    avcodec_free_context(&impl_->dec);
  if (impl_->pkt)
    av_packet_free(&impl_->pkt);
  if (impl_->frame)
    av_frame_free(&impl_->frame);
  impl_.reset();
}

bool CameraMic::readBlock()
{
  if (!impl_ || !impl_->fmt || !impl_->dec) {
    impl_->lastError = "mic not open";
    return false;
  }

  auto noAudioSince = std::chrono::steady_clock::now();
  while (impl_->pending8k.size() < kBlock8k) {
    const int ret = av_read_frame(impl_->fmt, impl_->pkt);
    if (ret < 0) {
      if (ret == AVERROR_EOF)
        impl_->lastError = "end of stream (camera closed the session)";
      else if (ret == AVERROR(EAGAIN))
        impl_->lastError = "audio timeout (2s)";
      else
        impl_->lastError = "read error";
      return false;
    }
    if (impl_->pkt->stream_index != impl_->audioIndex) {
      if (std::chrono::steady_clock::now() - noAudioSince >=
          std::chrono::milliseconds(1500)) {
        impl_->lastError = "the camera sends only video (no audio)";
        av_packet_unref(impl_->pkt);
        return false;
      }
      av_packet_unref(impl_->pkt);
      continue;
    }
    if (avcodec_send_packet(impl_->dec, impl_->pkt) == 0) {
      while (avcodec_receive_frame(impl_->dec, impl_->frame) == 0) {
        const int channels = impl_->frame->ch_layout.nb_channels;
        const int samples = impl_->frame->nb_samples;
        if (channels <= 0 || samples <= 0)
          continue;
        std::vector<int16_t> chunk;
        chunk.reserve(static_cast<size_t>(samples));
        const auto format = impl_->frame->format;
        if (format == AV_SAMPLE_FMT_S16 && channels == 1) {
          const auto* data =
              reinterpret_cast<const int16_t*>(impl_->frame->data[0]);
          chunk.assign(data, data + samples);
        }
        else if (format == AV_SAMPLE_FMT_S16P) {
          const auto* data =
              reinterpret_cast<const int16_t*>(impl_->frame->data[0]);
          for (int i = 0; i < samples; ++i)
            chunk.push_back(data[i]);
        }
        else if (format == AV_SAMPLE_FMT_U8 || format == AV_SAMPLE_FMT_U8P) {
          const auto* data =
              reinterpret_cast<const uint8_t*>(impl_->frame->data[0]);
          for (int i = 0; i < samples; ++i)
            chunk.push_back(
                static_cast<int16_t>((static_cast<int>(data[i]) - 128) << 8));
        }
        else {
          continue;
        }
        impl_->pending8k.insert(impl_->pending8k.end(), chunk.begin(),
                                chunk.end());
        noAudioSince = std::chrono::steady_clock::now();
        impl_->lastAudioAt = noAudioSince;
        impl_->deadline = impl_->lastAudioAt + impl_->recoverTimeout;
      }
    }
    av_packet_unref(impl_->pkt);
  }

  std::vector<int16_t> chunk(impl_->pending8k.begin(),
                             impl_->pending8k.begin() + kBlock8k);
  impl_->pending8k.erase(impl_->pending8k.begin(),
                         impl_->pending8k.begin() + kBlock8k);

  std::vector<int16_t> up;
  impl_->resampler->process(chunk.data(), chunk.size(), up);
  std::vector<float> out;
  out.reserve(up.size());
  int16_t peak = 1;
  for (const auto sample : up)
    peak = static_cast<int16_t>(
        std::max(static_cast<int>(peak), std::abs(static_cast<int>(sample))));
  const float target =
      std::clamp(0.8F * 32768.0F / static_cast<float>(peak), 1.0F, 4.0F);
  impl_->smoothGain = 0.5F * impl_->smoothGain + 0.5F * target;
  const float gain = impl_->smoothGain;
  for (const auto sample : up)
    out.push_back(
        std::clamp(static_cast<float>(sample) * gain / 32768.0F, -1.0F, 1.0F));
  if (impl_->onAudio)
    impl_->onAudio(out);
  return true;
}
