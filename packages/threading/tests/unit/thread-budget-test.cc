#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <algorithm>
#include <string>
#include <vector>

namespace
{

struct BudgetContract
{
    const char* name;
    int (*actual)();
    int (*scaled)(int hw);
};

} // namespace

TEST_CASE("every budget stays within its own clamp bounds")
{
    CHECK(ThreadBudget::hardwareThreads() >= 1);

    CHECK(ThreadBudget::computeThreads() >= 2);
    CHECK(ThreadBudget::computeThreads() <= 16);

    CHECK(ThreadBudget::batchThreads() >= 4);
    CHECK(ThreadBudget::batchThreads() <= 16);

    CHECK(ThreadBudget::heavyThreads() >= 2);
    CHECK(ThreadBudget::heavyThreads() <= 12);

    CHECK(ThreadBudget::lightThreads() >= 2);
    CHECK(ThreadBudget::lightThreads() <= 8);

    CHECK(ThreadBudget::inferenceSlots() >= 1);
    CHECK(ThreadBudget::inferenceSlots() <= 4);

    CHECK(ThreadBudget::ttsThreads() >= 2);
    CHECK(ThreadBudget::ttsThreads() <= 8);

    CHECK(ThreadBudget::extractionSlots() >= 1);
    CHECK(ThreadBudget::extractionSlots() <= 4);

    CHECK(ThreadBudget::extractionThreads() >= 1);
    CHECK(ThreadBudget::extractionThreads() <= 4);
}

TEST_CASE("each budget is monotonic non-decreasing in the hardware count")
{
    const std::vector<BudgetContract> contracts = {
        {"computeThreads", ThreadBudget::computeThreads,
         [](int hw) { return std::clamp(hw / 2, 2, 16); }},
        {"batchThreads", ThreadBudget::batchThreads,
         [](int hw) { return std::clamp(hw / 2, 4, 16); }},
        {"heavyThreads", ThreadBudget::heavyThreads,
         [](int hw) { return std::clamp(hw * 3 / 4, 2, 12); }},
        {"lightThreads", ThreadBudget::lightThreads,
         [](int hw) { return std::clamp(hw / 4, 2, 8); }},
        {"inferenceSlots", ThreadBudget::inferenceSlots,
         [](int hw) { return std::clamp(hw / 8, 1, 4); }},
        {"ttsThreads", ThreadBudget::ttsThreads,
         [](int hw) { return std::clamp(hw / 2, 2, 8); }},
        {"extractionSlots", ThreadBudget::extractionSlots,
         [](int hw) { return std::clamp(hw / 8, 1, 4); }},
        {"extractionThreads", ThreadBudget::extractionThreads,
         [](int hw) { return std::clamp(hw / 4, 1, 4); }},
    };

    for (const auto& contract : contracts) {
        INFO(contract.name);

        int previous = contract.scaled(1);
        for (int hw = 2; hw <= 256; ++hw) {
            const int current = contract.scaled(hw);
            CHECK(current >= previous);
            previous = current;
        }

        const int hw = ThreadBudget::hardwareThreads();
        CHECK(contract.actual() == contract.scaled(hw));
    }
}

TEST_CASE("queue workers are only raised for the extraction queue")
{
    CHECK(ThreadBudget::queueWorkers("memory.extract") ==
          ThreadBudget::extractionSlots());
    CHECK(ThreadBudget::queueWorkers("llm.prefill") == 1);
    CHECK(ThreadBudget::queueWorkers("") == 1);
}
