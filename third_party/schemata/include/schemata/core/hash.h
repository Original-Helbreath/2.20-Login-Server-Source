#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace schemata::detail {

/// Schemata's own 64-bit FNV-1a, rendered as sixteen hex digits.
///
/// Chosen for being short, dependency-free and stable across platforms and
/// runs. Nothing here is a security primitive: these hashes decide whether
/// something has changed since it was last looked at, and a collision costs
/// a skipped check rather than a wrong answer.
///
/// **Not the standard FNV-1a 64.** Its offset basis is 1469598103934665603,
/// a digit short of the standard 14695981039346656037, and every `add()` ends
/// with a separator byte, so that `{"ab", "c"}` and `{"a", "bc"}` do not hash
/// alike -- which matters when the pieces are names that sit next to each
/// other in a schema. It is kept as it is because every fingerprint and
/// content hash Schemata has recorded was taken with it: changing either
/// would read every recorded schema as changed and every migration as
/// edited. `fnv1a64_standard()` is the standard one.
class Fnv1a {
public:
    Fnv1a& add(std::string_view text) {
        for (const char c : text) {
            value_ ^= static_cast<unsigned char>(c);
            value_ *= prime;
        }
        value_ ^= 0xFFu;
        value_ *= prime;
        return *this;
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

    [[nodiscard]] std::string hex() const { return std::format("{:016x}", value_); }

private:
    static constexpr std::uint64_t offset = 1469598103934665603ULL;
    static constexpr std::uint64_t prime  = 1099511628211ULL;

    std::uint64_t value_ = offset;
};

/// The standard FNV-1a 64 of `bytes` -- offset basis 14695981039346656037,
/// prime 1099511628211, no separator -- as sixteen lowercase hex digits: `""`
/// is `cbf29ce484222325`, `"a"` is `af63dc4c8601ec8c`. For hashes a tool
/// outside Schemata took, where `Fnv1a` would not match them.
[[nodiscard]] inline std::string fnv1a64_standard(std::string_view bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const char c : bytes) {
        value ^= static_cast<unsigned char>(c);
        value *= 1099511628211ULL;
    }
    return std::format("{:016x}", value);
}

} // namespace schemata::detail
