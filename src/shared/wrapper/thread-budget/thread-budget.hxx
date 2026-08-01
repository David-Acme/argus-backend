#pragma once

#include <cstdint>

// Adaptive thread budgets so every AI service performs well on any machine,
// from 2-core laptops to 64-core servers, without hardcoded thread counts.
namespace ThreadBudget
{

// Total hardware threads (>= 1).
int hardwareThreads();

// Steady-state compute threads for heavy per-call inference (decode/synth).
// Roughly half the hardware threads, bounded to a sane range.
int computeThreads();

// Batch/prefill threads (parallel prompt/image processing).
// Usually larger than computeThreads since batch work scales better.
int batchThreads();

// Threads for large parallel kernels (vision encoder, graph models).
int heavyThreads();

// Lightweight tasks (tokenizers, small models).
int lightThreads();

// Max concurrent heavy inferences allowed across services.
// Scales with hardware so small machines stay responsive.
int inferenceSlots();

} // namespace ThreadBudget
