#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace schemata {

/// A UTC instant with microsecond resolution.
///
/// Microseconds rather than the `system_clock` default because that is what
/// the storage can actually hold: `DATETIME(6)` is the finest MySQL offers,
/// and pretending to nanoseconds would silently truncate on every write.
///
/// One of three aliases in this header, with `date` and `Blob` below. All
/// three exist because the underlying spellings are unreasonable to type,
/// not because Schemata wants types of its own.
using timestamp = std::chrono::sys_time<std::chrono::microseconds>;

/// A calendar date with no time and no zone.
using date = std::chrono::year_month_day;

/// Binary data. `std::byte` rather than `char` so that binary and text can
/// never be confused at the boundary.
using Blob = std::vector<std::byte>;

/// The storage classes a column can have, independent of engine.
///
/// This is the pivot of the type system: C++ types map to a ColumnType, and
/// each dialect renders a ColumnType as its own SQL. Neither side knows
/// about the other.
enum class ColumnType {
    boolean,

    // Signed integers, narrowest first. The order is load-bearing: widening
    // is decided by comparing these, so anything added has to keep it.
    int8,
    int16,
    int32,
    int64,

    // Unsigned, in the same order. Separate from the signed run rather than
    // interleaved with it, because signed-to-unsigned is never a widening in
    // either direction -- each can hold values the other cannot.
    uint8,
    uint16,
    uint32,
    uint64,

    real32,
    real,

    /// Exact decimal, sized by `Column::precision` and `Column::scale`.
    decimal,

    text,
    blob,
    timestamp,
    date,
};

/// Where a declared length stops meaning `VARCHAR(n)` on the MySQL family.
///
/// A row is capped at 65535 bytes and utf8mb4 spends four per character, so
/// beyond this a declared length selects a size variant instead. Not a
/// Schemata limit: the server refuses `VARCHAR(20000)` outright.
///
/// It is a ceiling on the *type*, not a length any real table can reach. The
/// row cap applies to the whole row, so `VARCHAR(16383)` fits only in a table
/// that has nothing else in it -- add a `BIGINT` key and the server refuses
/// the table. Around 16381 is the practical limit for a column with company,
/// and less again as the row grows.
inline constexpr int max_varchar_length = 16383;

/// Where a declared length stops meaning `VARBINARY(n)` on the MySQL family.
///
/// The same 65535-byte row as `max_varchar_length` guards, counted in the
/// unit the type uses: a `VARCHAR` measures characters and utf8mb4 spends
/// four on each, so 16383 characters and 65532 bytes are the same budget
/// written twice. The three bytes held back are the length prefix and the
/// null flag, which is why `VARBINARY(65535)` is not a column either server
/// will make -- MariaDB says so ("max = 65532; use BLOB or TEXT instead")
/// and MySQL says the row is too long.
///
/// Above this a declared length selects a size variant, exactly as it does
/// for text. Like `max_varchar_length` it is a ceiling on the type and not a
/// length a real table reaches: the row cap counts the whole row, so a
/// `VARBINARY(65532)` fits only in a table holding nothing else.
inline constexpr int max_varbinary_length = 65532;

/// The longest `CHAR(n)` or `BINARY(n)` the MySQL family will make.
///
/// A padded column is a different type from a varying one, with its own and
/// much lower ceiling: both servers refuse `CHAR(256)` and `BINARY(256)` with
/// "Column length too big for column (max = 255)". It is not tied to the row
/// size the way `max_varchar_length` is, so a fixed length between 256 and
/// that cut-over is not a longer column -- it is one no server will create.
inline constexpr int max_fixed_length = 255;

/// Storage ceilings for the TEXT and BLOB families, in bytes.
///
/// A declared length above `max_varchar_length` picks the narrowest of these
/// that holds it, which is what makes a scaffolded `LONGTEXT` render back as
/// `LONGTEXT` rather than silently truncating to 64KB.
inline constexpr int max_tinytext_length   = 255;
inline constexpr int max_text_length       = 65535;
inline constexpr int max_mediumtext_length = 16777215;

/// Fractional-second digits a `DATETIME` carries when nothing says otherwise.
///
/// Six, because that is the finest the MySQL family offers and what
/// `schemata::timestamp` holds. Declaring less is a deliberate narrowing; a
/// column that declares nothing keeps everything it is given.
inline constexpr int default_timestamp_precision = 6;

/// Total digits a `DECIMAL` carries when nothing says otherwise.
///
/// The MySQL family's own default for a bare `DECIMAL`, repeated here so that
/// a model and a server that both left it unsaid agree about what they meant.
inline constexpr int default_decimal_precision = 10;

/// The most decimal digits a `double` round-trips exactly.
///
/// IEEE-754 binary64 carries any 15-significant-digit decimal through to the
/// same 15 digits and back. Beyond that the conversion stops being reversible,
/// which is why `validate()` refuses a wider DECIMAL rather than storing one
/// that does not come back.
inline constexpr int max_exact_decimal_digits = 15;

/// The longest value the model can declare.
///
/// `LONGTEXT` really holds four gigabytes, which does not fit an `int`. Rather
/// than widen every length in the model for one ceiling nobody declares
/// deliberately, this is the point at which Schemata stops counting and simply
/// asks for the largest type there is. A column read back as `LONGTEXT`
/// reports this length, and a column declared this long renders as `LONGTEXT`,
/// so the round trip closes.
inline constexpr int max_declarable_length = 2147483647;

/// True for the integer types, signed or not. Not `boolean`, which is stored
/// as one but is not one.
[[nodiscard]] constexpr bool is_integer(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::int8:
        case ColumnType::int16:
        case ColumnType::int32:
        case ColumnType::int64:
        case ColumnType::uint8:
        case ColumnType::uint16:
        case ColumnType::uint32:
        case ColumnType::uint64: return true;
        default: return false;
    }
}

/// True for the integer types that cannot hold a negative value.
[[nodiscard]] constexpr bool is_unsigned(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::uint8:
        case ColumnType::uint16:
        case ColumnType::uint32:
        case ColumnType::uint64: return true;
        default: return false;
    }
}

/// How many bytes the engine gives an integer type, for deciding widening.
/// Zero for everything else.
[[nodiscard]] constexpr int integer_width(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::int8:
        case ColumnType::uint8: return 1;
        case ColumnType::int16:
        case ColumnType::uint16: return 2;
        case ColumnType::int32:
        case ColumnType::uint32: return 4;
        case ColumnType::int64:
        case ColumnType::uint64: return 8;
        default: return 0;
    }
}

[[nodiscard]] constexpr std::string_view to_string(ColumnType t) noexcept {
    switch (t) {
        case ColumnType::boolean:   return "boolean";
        case ColumnType::int8:      return "int8";
        case ColumnType::int16:     return "int16";
        case ColumnType::int32:     return "int32";
        case ColumnType::int64:     return "int64";
        case ColumnType::uint8:     return "uint8";
        case ColumnType::uint16:    return "uint16";
        case ColumnType::uint32:    return "uint32";
        case ColumnType::uint64:    return "uint64";
        case ColumnType::real32:    return "real32";
        case ColumnType::real:      return "real";
        case ColumnType::decimal:   return "decimal";
        case ColumnType::text:      return "text";
        case ColumnType::blob:      return "blob";
        case ColumnType::timestamp: return "timestamp";
        case ColumnType::date:      return "date";
    }
    return "unknown";
}

/// What a foreign key does when the referenced row goes away.
enum class ReferentialAction {
    no_action,
    restrict_,
    cascade,
    set_null,
    set_default,
};

[[nodiscard]] constexpr std::string_view to_sql(ReferentialAction a) noexcept {
    switch (a) {
        case ReferentialAction::no_action:   return "NO ACTION";
        case ReferentialAction::restrict_:   return "RESTRICT";
        case ReferentialAction::cascade:     return "CASCADE";
        case ReferentialAction::set_null:    return "SET NULL";
        case ReferentialAction::set_default: return "SET DEFAULT";
    }
    return "NO ACTION";
}

} // namespace schemata
