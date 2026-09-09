#pragma once

#include <cstdint>
#include <string>

// Adaptive thread budgets for every AI service, scaled to the host hardware.
namespace ThreadBudget
{

// Total hardware threads (>= 1).
int hardwareThreads();

// Steady-state compute threads for heavy per-call inference (decode/synth).
int computeThreads();

// Batch/prefill threads (parallel prompt/image processing).
int batchThreads();

// Threads for large parallel kernels (vision encoder, graph models).
int heavyThreads();

// Lightweight tasks (tokenizers, small models).
int lightThreads();

// Max concurrent heavy inferences allowed across services.
int inferenceSlots();

// Speech synthesis threads, capped lower than computeThreads.
int ttsThreads();

int extractionSlots();

int extractionThreads();

int queueWorkers(const std::string& queueName);

} // namespace ThreadBudget
