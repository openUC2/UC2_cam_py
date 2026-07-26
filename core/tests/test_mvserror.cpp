// Unit tests for the SDK error-code -> text helpers. No camera or SDK runtime
// state is required; these are pure string mappings.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "MvsError.h"
#include "MvErrorDefine.h"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;

TEST_CASE("mvsErrorString maps known codes", "[error]")
{
    CHECK(hik::mvsErrorString(MV_OK) == "success");
    CHECK(hik::mvsErrorString(MV_E_HANDLE) == "error or invalid handle");
    CHECK(hik::mvsErrorString(MV_E_PARAMETER) == "incorrect parameter");
}

TEST_CASE("mvsErrorString never returns an empty string", "[error]")
{
    // A code the switch does not handle still yields a non-empty fallback.
    const int unmapped = 0x12345678;
    CHECK_FALSE(hik::mvsErrorString(unmapped).empty());
    CHECK(hik::mvsErrorString(unmapped) == "unmapped error");
}

TEST_CASE("mvsErrorLabel appends the zero-padded hex code", "[error]")
{
    // Label = description + " (0x........)".
    CHECK_THAT(hik::mvsErrorLabel(MV_OK),
               ContainsSubstring("success") && EndsWith("(0x00000000)"));

    // The hex suffix reflects the raw (unsigned) code, 8 digits wide.
    const std::string label = hik::mvsErrorLabel(static_cast<int>(0x8000000cu));
    CHECK_THAT(label, EndsWith("(0x8000000c)"));
}
