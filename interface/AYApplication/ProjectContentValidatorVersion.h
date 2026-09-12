#pragma once

#include <cstdint>

// The validator returns public value types across a static-library boundary.
// Keep a source ABI number in every consumer so stale objects fail at link
// time after a public result/options layout change.
#ifndef AYAPPLICATION_CONTENT_VALIDATOR_SOURCE_ABI_VERSION
#define AYAPPLICATION_CONTENT_VALIDATOR_SOURCE_ABI_VERSION 2
#endif

static_assert(AYAPPLICATION_CONTENT_VALIDATOR_SOURCE_ABI_VERSION == 2,
              "Project content validator headers and target disagree; "
              "perform a full rebuild.");

#define AYAPPLICATION_CONTENT_VALIDATOR_STRINGIZE_IMPL(value) #value
#define AYAPPLICATION_CONTENT_VALIDATOR_STRINGIZE(value) \
    AYAPPLICATION_CONTENT_VALIDATOR_STRINGIZE_IMPL(value)
#if defined(_MSC_VER)
#pragma detect_mismatch( \
    "AYApplication.ProjectContentValidator.SourceABI", \
    AYAPPLICATION_CONTENT_VALIDATOR_STRINGIZE( \
        AYAPPLICATION_CONTENT_VALIDATOR_SOURCE_ABI_VERSION))
#endif

namespace ayt::app
{

inline constexpr std::uint32_t kProjectContentValidatorSourceAbiVersion =
    AYAPPLICATION_CONTENT_VALIDATOR_SOURCE_ABI_VERSION;

} // namespace ayt::app

#undef AYAPPLICATION_CONTENT_VALIDATOR_STRINGIZE
#undef AYAPPLICATION_CONTENT_VALIDATOR_STRINGIZE_IMPL
