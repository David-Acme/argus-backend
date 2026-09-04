#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <string>

TEST_CASE("thread budgets are never below one")
{
    CHECK(ThreadBudget::hardwareThreads() >= 1);
    CHECK(ThreadBudget::computeThreads() >= 1);
    CHECK(ThreadBudget::batchThreads() >= 1);
    CHECK(ThreadBudget::heavyThreads() >= 1);
    CHECK(ThreadBudget::lightThreads() >= 1);
    CHECK(ThreadBudget::inferenceSlots() >= 1);
    CHECK(ThreadBudget::ttsThreads() >= 1);
    CHECK(ThreadBudget::extractionSlots() >= 1);
    CHECK(ThreadBudget::extractionThreads() >= 1);
    CHECK(ThreadBudget::queueWorkers("memory.extract") >= 1);
    CHECK(ThreadBudget::queueWorkers("other.queue") >= 1);
}

TEST_CASE("light budgets stay within batch budgets")
{
    CHECK(ThreadBudget::lightThreads() <= ThreadBudget::batchThreads());
}

TEST_CASE("batch budgets stay within heavy budgets")
{
    CHECK(ThreadBudget::batchThreads() <= ThreadBudget::heavyThreads());
}

TEST_CASE("queue workers are only raised for the extraction queue")
{
    CHECK(ThreadBudget::queueWorkers("memory.extract") ==
          ThreadBudget::extractionSlots());
    CHECK(ThreadBudget::queueWorkers("llm.prefill") == 1);
    CHECK(ThreadBudget::queueWorkers("") == 1);
}