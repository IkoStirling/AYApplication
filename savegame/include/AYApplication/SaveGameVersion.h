#pragma once

#include <cstdint>

// SaveGame public value types cross the static-library boundary. Keep a
// source ABI number in consumers so stale objects fail at link time instead
// of interpreting changed result or migration layouts.
#ifndef AYAPPLICATION_SAVEGAME_SOURCE_ABI_VERSION
#define AYAPPLICATION_SAVEGAME_SOURCE_ABI_VERSION 1
#endif

static_assert(AYAPPLICATION_SAVEGAME_SOURCE_ABI_VERSION == 1,
              "SaveGame headers and target disagree; perform a full rebuild.");

#define AYAPPLICATION_SAVEGAME_STRINGIZE_IMPL(value) #value
#define AYAPPLICATION_SAVEGAME_STRINGIZE(value) \
    AYAPPLICATION_SAVEGAME_STRINGIZE_IMPL(value)
#if defined(_MSC_VER)
#pragma detect_mismatch( \
    "AYApplication.SaveGame.SourceABI", \
    AYAPPLICATION_SAVEGAME_STRINGIZE( \
        AYAPPLICATION_SAVEGAME_SOURCE_ABI_VERSION))
#endif

namespace ayt::app
{

inline constexpr std::uint32_t kSaveGameSourceAbiVersion =
    AYAPPLICATION_SAVEGAME_SOURCE_ABI_VERSION;

} // namespace ayt::app

#undef AYAPPLICATION_SAVEGAME_STRINGIZE
#undef AYAPPLICATION_SAVEGAME_STRINGIZE_IMPL
