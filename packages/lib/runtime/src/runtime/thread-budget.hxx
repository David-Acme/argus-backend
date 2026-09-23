#pragma once

#include <cstdint>
#include <string>

namespace ThreadBudget
{

int hardwareThreads();

int computeThreads();

int batchThreads();

int heavyThreads();

int lightThreads();

int inferenceSlots();

int ttsThreads();

int extractionSlots();

int extractionThreads();

int queueWorkers(const std::string& queueName);

}
