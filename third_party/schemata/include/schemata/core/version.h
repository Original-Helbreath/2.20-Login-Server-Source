#pragma once

#include <cstdint>
#include <string_view>

namespace schemata {

inline constexpr int version_major = 0;
inline constexpr int version_minor = 1;
inline constexpr int version_patch = 0;

/// Human-readable library version, e.g. "0.1.0".
[[nodiscard]] std::string_view version() noexcept;

} // namespace schemata
