#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sync/stream-retention.hxx>

TEST_CASE("the feed window is seven days in every unit")
{
    CHECK(stream_retention::kRetentionMs == 604800000);
    CHECK(stream_retention::kRetentionSeconds == 604800);
    CHECK(stream_retention::kRetentionNs == 604800000000000);
    CHECK(stream_retention::kRetentionSeconds * 1000 ==
          stream_retention::kRetentionMs);
    CHECK(stream_retention::kRetentionNs / 1000000 ==
          stream_retention::kRetentionMs);
}

TEST_CASE("the duplicate window and the settled-row cadences hold their values")
{
    CHECK(stream_retention::kDuplicatesNs == 120000000000);
    CHECK(stream_retention::kSettledPurgeIntervalMs == 86400000);
    CHECK(stream_retention::kSettledPurgeRetryMs == 3600000);
    CHECK(stream_retention::kSettledPurgeIntervalMs <
          stream_retention::kRetentionMs);
    CHECK(stream_retention::kSettledPurgeRetryMs <
          stream_retention::kSettledPurgeIntervalMs);
}
