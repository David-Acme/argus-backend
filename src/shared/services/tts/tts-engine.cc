#include "tts-engine.hxx"

#include "onnx-utils.hxx"
#include "style.hxx"
#include "unicode-processor.hxx"

#include <algorithm>
#include <stdexcept>

TtsEngine::TtsEngine(const Config& cfg, UnicodeProcessor* processor,
                     std::unique_ptr<Ort::Session> dp,
                     std::unique_ptr<Ort::Session> textEnc,
                     std::unique_ptr<Ort::Session> vectorEst,
                     std::unique_ptr<Ort::Session> vocoder,
                     Ort::MemoryInfo&& memoryInfo)
    : cfg_(cfg), processor_(processor), dp_(std::move(dp)),
      textEnc_(std::move(textEnc)), vectorEst_(std::move(vectorEst)),
      vocoder_(std::move(vocoder)), memoryInfo_(std::move(memoryInfo))
{
  sampleRate_ = cfg_.ae.sampleRate;
  baseChunkSize_ = cfg_.ae.baseChunkSize;
  chunkCompressFactor_ = cfg_.ttl.chunkCompressFactor;
  ldim_ = cfg_.ttl.latentDim;
}

void TtsEngine::sampleNoisyLatent(
    const std::vector<float>& duration,
    std::vector<std::vector<std::vector<float>>>& noisyLatent,
    std::vector<std::vector<std::vector<float>>>& latentMaskOut) const
{
  int bsz = static_cast<int>(duration.size());
  float wavLenMax =
      *std::max_element(duration.begin(), duration.end()) * sampleRate_;

  std::vector<int64_t> wavLengths;
  wavLengths.reserve(bsz);
  for (float d : duration) {
    wavLengths.push_back(static_cast<int64_t>(d * sampleRate_));
  }

  int chunkSize = baseChunkSize_ * chunkCompressFactor_;
  int latentLen = static_cast<int>((wavLenMax + chunkSize - 1) / chunkSize);
  int latentDim = ldim_ * chunkCompressFactor_;

  noisyLatent.resize(bsz);
  for (int b = 0; b < bsz; b++) {
    noisyLatent[b].resize(latentDim);
    for (int d = 0; d < latentDim; d++) {
      noisyLatent[b][d].resize(latentLen);
      for (int t = 0; t < latentLen; t++) {
        noisyLatent[b][d][t] = noiseDist_(rng_);
      }
    }
  }

  latentMaskOut = latentMask(wavLengths, baseChunkSize_, chunkCompressFactor_);

  for (int b = 0; b < bsz; b++) {
    for (int d = 0; d < latentDim; d++) {
      for (size_t t = 0; t < noisyLatent[b][d].size(); t++) {
        noisyLatent[b][d][t] *= latentMaskOut[b][0][t];
      }
    }
  }
}

TtsEngine::Result TtsEngine::infer(const std::vector<std::string>& textList,
                                   const std::vector<std::string>& langList,
                                   const Style& style, int totalStep,
                                   float speed) const
{
  tensorFloats_.clear();
  tensorInts_.clear();

  int bsz = static_cast<int>(textList.size());

  if (bsz != style.ttlShape()[0]) {
    throw std::runtime_error(
        "Number of texts must match number of style vectors");
  }

  // --- Process text ---
  std::vector<std::vector<int64_t>> textIds;
  std::vector<std::vector<std::vector<float>>> textMask;
  processor_->process(textList, langList, textIds, textMask);

  std::vector<int64_t> textIdsShape = {bsz,
                                       static_cast<int64_t>(textIds[0].size())};
  std::vector<int64_t> textMaskShape = {bsz, 1,
                                        static_cast<int64_t>(
                                            textMask[0][0].size())};

  // --- Build tensors once ---
  Ort::Value textIdsTensor =
      intArrayToTensor(memoryInfo_, textIds, textIdsShape, tensorInts_);
  Ort::Value textMaskTensor =
      arrayToTensor(memoryInfo_, textMask, textMaskShape, tensorFloats_);

  Ort::Value styleTtlTensor =
      Ort::Value::CreateTensor<float>(memoryInfo_,
                                      const_cast<float*>(
                                          style.ttlData().data()),
                                      style.ttlData().size(),
                                      style.ttlShape().data(),
                                      style.ttlShape().size());

  Ort::Value styleDpTensor =
      Ort::Value::CreateTensor<float>(memoryInfo_,
                                      const_cast<float*>(style.dpData().data()),
                                      style.dpData().size(),
                                      style.dpShape().data(),
                                      style.dpShape().size());

  // --- Duration predictor ---
  const char* dpInputNames[] = {"text_ids", "style_dp", "text_mask"};
  const char* dpOutputNames[] = {"duration"};
  std::vector<Ort::Value> dpInputs;
  dpInputs.reserve(3);
  dpInputs.push_back(std::move(textIdsTensor));
  dpInputs.push_back(std::move(styleDpTensor));
  dpInputs.push_back(std::move(textMaskTensor));

  auto dpOutputs = dp_->Run(Ort::RunOptions{nullptr}, dpInputNames,
                            dpInputs.data(), dpInputs.size(), dpOutputNames, 1);

  auto* durData = dpOutputs[0].GetTensorMutableData<float>();
  std::vector<float> duration(durData, durData + bsz);

  for (auto& dur : duration) {
    dur /= speed;
  }

  // --- Rebuild tensors for text encoder (previous ones were moved) ---
  Ort::Value textIdsTensor2 =
      intArrayToTensor(memoryInfo_, textIds, textIdsShape, tensorInts_);
  Ort::Value textMaskTensor2 =
      arrayToTensor(memoryInfo_, textMask, textMaskShape, tensorFloats_);

  Ort::Value styleTtlTensor2 =
      Ort::Value::CreateTensor<float>(memoryInfo_,
                                      const_cast<float*>(
                                          style.ttlData().data()),
                                      style.ttlData().size(),
                                      style.ttlShape().data(),
                                      style.ttlShape().size());

  // --- Text encoder ---
  const char* textEncInputNames[] = {"text_ids", "style_ttl", "text_mask"};
  const char* textEncOutputNames[] = {"text_emb"};
  std::vector<Ort::Value> textEncInputs;
  textEncInputs.reserve(3);
  textEncInputs.push_back(std::move(textIdsTensor2));
  textEncInputs.push_back(std::move(styleTtlTensor2));
  textEncInputs.push_back(std::move(textMaskTensor2));

  auto textEncOutputs =
      textEnc_->Run(Ort::RunOptions{nullptr}, textEncInputNames,
                    textEncInputs.data(), textEncInputs.size(),
                    textEncOutputNames, 1);

  // --- Sample noisy latent ---
  std::vector<std::vector<std::vector<float>>> xt, latentMaskData;
  sampleNoisyLatent(duration, xt, latentMaskData);

  std::vector<int64_t> latentShape = {bsz, static_cast<int64_t>(xt[0].size()),
                                      static_cast<int64_t>(xt[0][0].size())};
  std::vector<int64_t> latentMaskShape = {bsz, 1,
                                          static_cast<int64_t>(
                                              latentMaskData[0][0].size())};

  // --- Cache text_emb for reuse across iterations ---
  auto textEmbInfo = textEncOutputs[0].GetTensorTypeAndShapeInfo();
  size_t textEmbSize = textEmbInfo.GetElementCount();
  auto* textEmbDataPtr = textEncOutputs[0].GetTensorMutableData<float>();
  std::vector<float> textEmbVec(textEmbDataPtr, textEmbDataPtr + textEmbSize);
  auto textEmbShape = textEmbInfo.GetShape();

  // --- Prepare scalar tensors ---
  std::vector<float> totalStepVec(bsz, static_cast<float>(totalStep));
  Ort::Value totalStepTensor =
      Ort::Value::CreateTensor<float>(memoryInfo_, totalStepVec.data(),
                                      totalStepVec.size(),
                                      std::vector<int64_t>{bsz}.data(), 1);

  // --- Iterative denoising ---
  for (int step = 0; step < totalStep; step++) {
    std::vector<float> currentStepVec(bsz, static_cast<float>(step));

    // Recreate source tensors for this iteration
    Ort::Value textMaskTensorIter =
        arrayToTensor(memoryInfo_, textMask, textMaskShape, tensorFloats_);
    Ort::Value latentMaskTensor = arrayToTensor(memoryInfo_, latentMaskData,
                                                latentMaskShape, tensorFloats_);
    Ort::Value noisyLatentTensor =
        arrayToTensor(memoryInfo_, xt, latentShape, tensorFloats_);

    Ort::Value styleTtlTensorIter =
        Ort::Value::CreateTensor<float>(memoryInfo_,
                                        const_cast<float*>(
                                            style.ttlData().data()),
                                        style.ttlData().size(),
                                        style.ttlShape().data(),
                                        style.ttlShape().size());

    Ort::Value textEmbTensor =
        Ort::Value::CreateTensor<float>(memoryInfo_, textEmbVec.data(),
                                        textEmbVec.size(), textEmbShape.data(),
                                        textEmbShape.size());

    Ort::Value currentStepTensor =
        Ort::Value::CreateTensor<float>(memoryInfo_, currentStepVec.data(),
                                        currentStepVec.size(),
                                        std::vector<int64_t>{bsz}.data(), 1);

    Ort::Value totalStepTensorIter =
        Ort::Value::CreateTensor<float>(memoryInfo_, totalStepVec.data(),
                                        totalStepVec.size(),
                                        std::vector<int64_t>{bsz}.data(), 1);

    const char* vectorEstInputNames[] = {"noisy_latent", "text_emb",
                                         "style_ttl",    "text_mask",
                                         "latent_mask",  "total_step",
                                         "current_step"};
    const char* vectorEstOutputNames[] = {"denoised_latent"};

    std::vector<Ort::Value> vectorEstInputs;
    vectorEstInputs.reserve(7);
    vectorEstInputs.push_back(std::move(noisyLatentTensor));
    vectorEstInputs.push_back(std::move(textEmbTensor));
    vectorEstInputs.push_back(std::move(styleTtlTensorIter));
    vectorEstInputs.push_back(std::move(textMaskTensorIter));
    vectorEstInputs.push_back(std::move(latentMaskTensor));
    vectorEstInputs.push_back(std::move(totalStepTensorIter));
    vectorEstInputs.push_back(std::move(currentStepTensor));

    auto vectorEstOutputs =
        vectorEst_->Run(Ort::RunOptions{nullptr}, vectorEstInputNames,
                        vectorEstInputs.data(), vectorEstInputs.size(),
                        vectorEstOutputNames, 1);

    auto* denoisedData = vectorEstOutputs[0].GetTensorMutableData<float>();
    size_t idx = 0;
    for (int b = 0; b < bsz; b++) {
      for (size_t d = 0; d < xt[b].size(); d++) {
        for (size_t t = 0; t < xt[b][d].size(); t++) {
          xt[b][d][t] = denoisedData[idx++];
        }
      }
    }
  }

  // --- Vocoder ---
  Ort::Value latentTensor =
      arrayToTensor(memoryInfo_, xt, latentShape, tensorFloats_);
  const char* vocoderInputNames[] = {"latent"};
  const char* vocoderOutputNames[] = {"wav_tts"};
  std::vector<Ort::Value> vocoderInputs;
  vocoderInputs.push_back(std::move(latentTensor));

  auto vocoderOutputs =
      vocoder_->Run(Ort::RunOptions{nullptr}, vocoderInputNames,
                    vocoderInputs.data(), vocoderInputs.size(),
                    vocoderOutputNames, 1);

  auto wavInfo = vocoderOutputs[0].GetTensorTypeAndShapeInfo();
  size_t wavSize = wavInfo.GetElementCount();
  auto* wavData = vocoderOutputs[0].GetTensorMutableData<float>();

  Result result;
  result.wav.assign(wavData, wavData + wavSize);
  result.duration = std::move(duration);

  return result;
}

TtsEngine::Result TtsEngine::synthesize(const std::string& text,
                                        const std::string& lang,
                                        const Style& style, int totalStep,
                                        float speed) const
{
  if (style.ttlShape()[0] != 1) {
    throw std::runtime_error(
        "Single speaker text to speech only supports single style");
  }

  int maxLen = (lang == "ko" || lang == "ja") ? 120 : 300;
  auto textList = chunkText(text, maxLen);

  std::vector<float> wavCat;
  float durCat = 0.0f;

  for (const auto& chunk : textList) {
    auto result = infer({chunk}, {lang}, style, totalStep, speed);

    if (wavCat.empty()) {
      wavCat = std::move(result.wav);
      durCat = result.duration[0];
    }
    else {
      int silenceLen = static_cast<int>(0.3f * sampleRate_);
      std::vector<float> silence(silenceLen, 0.0f);
      wavCat.insert(wavCat.end(), silence.begin(), silence.end());
      wavCat.insert(wavCat.end(), result.wav.begin(), result.wav.end());
      durCat += result.duration[0] + 0.3f;
    }
  }

  Result finalResult;
  finalResult.wav = std::move(wavCat);
  finalResult.duration = {durCat};

  return finalResult;
}
