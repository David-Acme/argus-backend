#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sync/resource-change.hxx>

#include <algorithm>
#include <cstddef>

TEST_CASE("the resource vocabulary round-trips and answers nothing unknown")
{
    for (int value = 0; value <= static_cast<int>(ResourceDomain::Activity); ++value) {
        const auto domain = static_cast<ResourceDomain>(value);
        CHECK(resourceDomainFromString(resourceDomainToString(domain)) == domain);
    }
    for (int value = 0; value <= static_cast<int>(ResourceScope::Clips); ++value) {
        const auto scope = static_cast<ResourceScope>(value);
        CHECK(resourceScopeFromString(resourceScopeToString(scope)) == scope);
    }
    CHECK(resourceDomainToString(ResourceDomain::Guard) == "guard");
    CHECK(resourceScopeToString(ResourceScope::ExpectedVisits) == "expected_visits");
    CHECK_FALSE(resourceDomainFromString("gate").has_value());
    CHECK_FALSE(resourceScopeFromString("guests").has_value());
}

TEST_CASE("every scope carries exactly one read route and every domain is used")
{
    for (int value = 0; value <= static_cast<int>(ResourceScope::Clips); ++value) {
        const auto scope = static_cast<ResourceScope>(value);
        const auto rows = std::ranges::count_if(
            kResourceRoutes, [scope](const ResourceRoute& route) {
                return route.scope == scope;
            });
        CHECK(rows == 1);
    }
    for (int value = 0; value <= static_cast<int>(ResourceDomain::Activity); ++value) {
        const auto domain = static_cast<ResourceDomain>(value);
        CHECK(std::ranges::any_of(
            kResourceRoutes, [domain](const ResourceRoute& route) {
                return route.domain == domain;
            }));
    }
}

TEST_CASE("no two rows carry the same domain and scope")
{
    for (std::size_t left = 0; left < kResourceRoutes.size(); ++left) {
        for (std::size_t right = left + 1; right < kResourceRoutes.size(); ++right) {
            const bool same = kResourceRoutes[left].domain == kResourceRoutes[right].domain &&
                              kResourceRoutes[left].scope == kResourceRoutes[right].scope;
            CHECK_FALSE(same);
        }
    }
}

TEST_CASE("every route resolves through the registry and reads")
{
    for (const auto& route : kResourceRoutes) {
        const ResourceRoute* found = resourceRouteOf(route.domain, route.scope);
        REQUIRE(found != nullptr);
        CHECK(found->method == drogon::Get);
        CHECK_FALSE(found->path.empty());
    }
}
