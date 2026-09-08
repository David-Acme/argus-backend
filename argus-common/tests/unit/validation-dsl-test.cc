#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <shared/validation/validator.hxx>
#include <string>
#include <vector>

struct ValidationProbe
{
    std::string name;
    std::string email;
    std::string role;
    std::string password;
    std::string confirmation;
    std::optional<std::string> nickname;
    int64_t timestamp = 0;
    std::optional<int64_t> expiresAt;
    bool active = false;
    std::vector<std::string> tags;
};

struct FormatProbe
{
    std::string url;
    std::string hex;
    std::string slug;
    std::string base64;
    std::string alpha;
    std::string alnum;
    std::string compact;
    std::string code;
    std::string uuid;
    int64_t count = 0;
    int64_t lower = 0;
    int64_t upper = 0;
    int64_t offset = 0;
    bool flag = false;
};

static void validateProbe(const ValidationProbe& probe)
{
    START_VALIDATION(ValidationProbe, probe)
    IS_NOT_EMPTY(name)
    IS_EMAIL(email)
    IS_IN(role, "owner", "resident", "guard", "guest")
    MIN_LENGTH(password, 8)
    MAX_LENGTH(password, 64)
    EQUALS_FIELD(confirmation, password)
    IS_NOT_EMPTY_OPTIONAL(nickname)
    MIN_LENGTH_OPTIONAL(nickname, 2)
    MAX_LENGTH_OPTIONAL(nickname, 32)
    IS_VALID_TIMESTAMP(timestamp)
    IS_POSITIVE_TIMESTAMP_OPTIONAL(expiresAt)
    ARRAY_NOT_EMPTY(tags, std::string)
    MIN_ELEMENTS(tags, std::string, 1)
    MAX_ELEMENTS(tags, std::string, 5)
    CUSTOM_LAMBDA(active,
                  [](const ValidationProbe& probe) -> std::optional<std::string> {
                      if (!probe.active)
                          return "active must be true";
                      return std::nullopt;
                  })
    END_VALIDATION()
}

static void validateFormat(const FormatProbe& probe)
{
    START_VALIDATION(FormatProbe, probe)
    IS_UUID(uuid)
    IS_URL(url)
    IS_HEX(hex)
    IS_SLUG(slug)
    IS_BASE64(base64)
    IS_ALPHA(alpha)
    IS_ALNUM(alnum)
    HAS_NO_SPACES(compact)
    MATCHES_REGEX(code, "^[A-Z]{3}-[0-9]{4}$", "must match the code pattern")
    IS_POSITIVE(count)
    IS_NON_NEGATIVE(offset)
    MIN_INT(lower, -10)
    MAX_INT(upper, 10)
    BETWEEN(count, 3, 10)
    IS_BOOLEAN(flag)
    END_VALIDATION()
}

static std::optional<ValidationErrors> probeErrors(const ValidationProbe& probe)
{
    try {
        validateProbe(probe);
        return std::nullopt;
    } catch (const ValidationException& e) {
        return e.errors();
    }
}

static std::optional<ValidationErrors> formatErrors(const FormatProbe& probe)
{
    try {
        validateFormat(probe);
        return std::nullopt;
    } catch (const ValidationException& e) {
        return e.errors();
    }
}

static ValidationProbe validProbe()
{
    ValidationProbe probe;
    probe.name = "Alice";
    probe.email = "alice@example.com";
    probe.role = "resident";
    probe.password = "long-enough-password";
    probe.confirmation = "long-enough-password";
    probe.nickname = "ali";
    probe.timestamp = 1700000000;
    probe.expiresAt = 1800000000;
    probe.active = true;
    probe.tags = {"home", "front-door"};
    return probe;
}

static FormatProbe validFormat()
{
    FormatProbe probe;
    probe.url = "https://argus.local/health";
    probe.hex = "deadBEEF00";
    probe.slug = "front-door-camera";
    probe.base64 = "aGVsbG8=";
    probe.alpha = "Argus";
    probe.alnum = "cam42";
    probe.compact = "no-spaces-here";
    probe.code = "ABC-1234";
    probe.uuid = "123e4567-e89b-12d3-a456-426614174000";
    probe.count = 3;
    probe.lower = -5;
    probe.upper = 4;
    probe.offset = 2;
    probe.flag = true;
    return probe;
}

TEST_CASE("validation-dsl accepts a fully valid dto")
{
    CHECK_FALSE(probeErrors(validProbe()).has_value());
    CHECK_FALSE(formatErrors(validFormat()).has_value());
}

TEST_CASE("validation-dsl reports empty required fields")
{
    auto probe = validProbe();
    probe.name = "";
    const auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    REQUIRE(errors->at("name").size() == 1);
    CHECK(errors->at("name")[0] == "name must not be empty");
}

TEST_CASE("validation-dsl reports email format failures")
{
    auto probe = validProbe();
    probe.email = "not-an-email";
    auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    REQUIRE(errors->at("email").size() == 1);
    CHECK(errors->at("email")[0] == "email must be a valid email");

    probe.email = "missing@tld";
    errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("email")[0] == "email must be a valid email");
}

TEST_CASE("validation-dsl restricts IS_IN to allowed values")
{
    auto probe = validProbe();
    probe.role = "superadmin";
    const auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    REQUIRE(errors->at("role").size() == 1);
    CHECK(errors->at("role")[0] == "role must be one of the allowed values");

    for (const auto& allowed : {"owner", "resident", "guard", "guest"}) {
        probe.role = allowed;
        CHECK_FALSE(probeErrors(probe).has_value());
    }
}

TEST_CASE("validation-dsl enforces MIN_LENGTH and MAX_LENGTH bounds")
{
    auto probe = validProbe();
    probe.password = "short";
    auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("password")[0] == "password must be at least 8 characters");

    probe.password = std::string(65, 'x');
    probe.confirmation = probe.password;
    errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("password")[0] == "password must be at most 64 characters");
}

TEST_CASE("validation-dsl cross-checks EQUALS_FIELD")
{
    auto probe = validProbe();
    probe.confirmation = "different-password";
    const auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("confirmation")[0] == "confirmation must equal password");
}

TEST_CASE("validation-dsl treats optional string fields correctly")
{
    auto probe = validProbe();
    probe.nickname = "";
    auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("nickname")[0] == "nickname must not be empty");

    probe.nickname = "a";
    errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("nickname")[0] == "nickname must be at least 2 characters");

    probe.nickname = std::string(33, 'n');
    errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("nickname")[0] == "nickname must be at most 32 characters");

    probe.nickname = std::nullopt;
    CHECK_FALSE(probeErrors(probe).has_value());
}

TEST_CASE("validation-dsl validates timestamps")
{
    auto probe = validProbe();
    probe.timestamp = 0;
    auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("timestamp")[0] == "timestamp must be a valid timestamp");

    probe = validProbe();
    probe.expiresAt = 0;
    errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("expiresAt")[0] == "expiresAt must be a positive timestamp");

    probe = validProbe();
    probe.expiresAt = -42;
    errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("expiresAt")[0] == "expiresAt must be a positive timestamp");

    probe = validProbe();
    probe.expiresAt = std::nullopt;
    CHECK_FALSE(probeErrors(probe).has_value());
}

TEST_CASE("validation-dsl enforces array rules")
{
    auto probe = validProbe();
    probe.tags.clear();
    auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    REQUIRE(errors->at("tags").size() == 2);
    CHECK(errors->at("tags")[0] == "tags must not be empty");
    CHECK(errors->at("tags")[1] == "tags must have at least 1 elements");

    probe.tags = {"a", "b", "c", "d", "e", "f"};
    errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("tags")[0] == "tags must have at most 5 elements");
}

TEST_CASE("validation-dsl runs CUSTOM_LAMBDA")
{
    auto probe = validProbe();
    probe.active = false;
    const auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    REQUIRE(errors->at("active").size() == 1);
    CHECK(errors->at("active")[0] == "active must be true");
}

TEST_CASE("validation-dsl accumulates errors from several fields")
{
    auto probe = validProbe();
    probe.name = "";
    probe.email = "broken";
    probe.active = false;
    const auto errors = probeErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->size() == 3);
    CHECK(errors->count("name") == 1);
    CHECK(errors->count("email") == 1);
    CHECK(errors->count("active") == 1);
}

TEST_CASE("validation-dsl throws ValidationException with 422 and the field-error map")
{
    const auto probe = validProbe();
    bool threw = false;
    try {
        validateProbe(probe);
    } catch (const ValidationException&) {
        threw = true;
    }
    CHECK_FALSE(threw);

    auto broken = validProbe();
    broken.email = "broken";
    broken.nickname = "";
    try {
        validateProbe(broken);
        CHECK_FALSE(true);
    } catch (const ValidationException& e) {
        CHECK(e.statusCode() == 422);
        CHECK(e.errors().at("email").size() == 1);
        CHECK(e.errors().at("nickname").size() == 2);
    }
}

TEST_CASE("format rules accept well-formed values")
{
    CHECK_FALSE(formatErrors(validFormat()).has_value());
}

TEST_CASE("format rules reject malformed values")
{
    auto probe = validFormat();
    probe.uuid = "not-a-uuid";
    auto errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("uuid")[0] == "uuid must be a valid UUID");

    probe = validFormat();
    probe.url = "ftp://argus.local";
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("url")[0] == "url must be a valid URL");

    probe = validFormat();
    probe.hex = "xyz";
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("hex")[0] == "hex must be a valid hex string");

    probe = validFormat();
    probe.slug = "Front Door";
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("slug")[0] == "slug must be a valid slug");

    probe = validFormat();
    probe.base64 = "abcde";
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("base64")[0] == "base64 must be a valid base64 string");

    probe = validFormat();
    probe.alpha = "Argus42";
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("alpha")[0] == "alpha must contain only letters");

    probe = validFormat();
    probe.alnum = "cam-42";
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("alnum")[0] == "alnum must contain only letters and digits");

    probe = validFormat();
    probe.compact = "has space";
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("compact")[0] == "compact must not contain spaces");
}

TEST_CASE("MATCHES_REGEX enforces the given pattern")
{
    auto probe = validFormat();
    probe.code = "12-3456";
    const auto errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("code")[0] == "code must match the code pattern");
}

TEST_CASE("numeric rules bound int64 fields")
{
    auto probe = validFormat();
    probe.count = 0;
    auto errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("count")[0] == "count must be positive");

    probe = validFormat();
    probe.count = 11;
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("count")[0] == "count must be between 3 and 10");

    probe = validFormat();
    probe.count = 2;
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("count")[0] == "count must be between 3 and 10");

    probe = validFormat();
    probe.lower = -11;
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("lower")[0] == "lower must be at least -10");

    probe = validFormat();
    probe.upper = 11;
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("upper")[0] == "upper must be at most 10");

    probe = validFormat();
    probe.offset = -1;
    errors = formatErrors(probe);
    REQUIRE(errors.has_value());
    CHECK(errors->at("offset")[0] == "offset must be non-negative");

    probe = validFormat();
    probe.lower = -10;
    probe.upper = 10;
    probe.count = 10;
    CHECK_FALSE(formatErrors(probe).has_value());
}

TEST_CASE("ValidationException defaults to 422")
{
    const ValidationErrors errors{{"field", {"must not be empty"}}};
    const ValidationException exception(errors);
    CHECK(exception.statusCode() == 422);
    CHECK(exception.errors().at("field").size() == 1);
    CHECK(exception.errors().at("field")[0] == "must not be empty");
}
