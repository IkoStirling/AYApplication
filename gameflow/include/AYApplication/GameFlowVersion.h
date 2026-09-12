#pragma once

#include <cstdint>

// GameFlow is linked statically today, but its public value types cross target
// boundaries. Keep a source ABI number in every consumer so future stale
// object mixes fail at link time instead of corrupting public struct layouts.
#ifndef AYAPPLICATION_GAMEFLOW_SOURCE_ABI_VERSION
#define AYAPPLICATION_GAMEFLOW_SOURCE_ABI_VERSION 2
#endif

static_assert(AYAPPLICATION_GAMEFLOW_SOURCE_ABI_VERSION == 2,
              "GameFlow headers and target disagree; perform a full rebuild.");

#define AYAPPLICATION_GAMEFLOW_STRINGIZE_IMPL(value) #value
#define AYAPPLICATION_GAMEFLOW_STRINGIZE(value) \
    AYAPPLICATION_GAMEFLOW_STRINGIZE_IMPL(value)
#if defined(_MSC_VER)
#pragma detect_mismatch( \
    "AYApplication.GameFlow.SourceABI", \
    AYAPPLICATION_GAMEFLOW_STRINGIZE( \
        AYAPPLICATION_GAMEFLOW_SOURCE_ABI_VERSION))
#endif

namespace ayt::app
{

inline constexpr std::uint32_t kGameFlowSourceAbiVersion =
    AYAPPLICATION_GAMEFLOW_SOURCE_ABI_VERSION;

} // namespace ayt::app

#undef AYAPPLICATION_GAMEFLOW_STRINGIZE
#undef AYAPPLICATION_GAMEFLOW_STRINGIZE_IMPL
