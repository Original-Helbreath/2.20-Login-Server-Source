#pragma once

#include <cstdint>

namespace schemata {

/// Which rows of the right-hand table a join keeps.
///
/// Shared by the typed join of `<schemata/query/join.h>` (`JoinQuery`) and the
/// join by table and column name of `<schemata/query/rows.h>` (`JoinSpec`).
enum class JoinKind : std::uint8_t {
    /// Only pairs the `ON` condition matches.
    inner,

    /// Every left row, with the right one absent where nothing matched.
    left,
};

} // namespace schemata
