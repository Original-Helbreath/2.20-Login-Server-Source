#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <type_traits>

#include <schemata/core/error.h>
#include <schemata/core/types.h>
#include <schemata/core/value.h>

namespace schemata {

/// How a C++ type becomes a column value and back.
///
/// The primary template is deliberately undefined: a field whose type has no
/// converter is a compile error naming that type, rather than a silent
/// fallback to something wrong.
///
/// Specialise it to store an enum, a UUID, a JSON wrapper or anything else
/// as one column:
///
///     template <> struct schemata::ValueConverter<Colour> {
///         static constexpr ColumnType column_type() { return ColumnType::text; }
///         static constexpr bool nullable() { return false; }
///         static Value to_value(const Colour& c) { return to_string(c); }
///         static void from_value(const Value& v, Colour& c) { c = parse(v.as_text()); }
///     };
///
/// This is the extension point that keeps D7 honest: user types are handled
/// without Schemata inventing library types for them.
template <typename T, typename Enable = void>
struct ValueConverter;

// ---------------------------------------------------------------------------
// Built-ins
// ---------------------------------------------------------------------------

template <>
struct ValueConverter<bool> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::boolean; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value                to_value(bool v) { return Value{v}; }
    static void                 from_value(const Value& v, bool& out) {
        // Engines disagree about how a boolean comes back: SQLite and MySQL
        // both store it as an integer, and a driver that fetched it as one
        // hands over an int64 rather than a bool.
        if (v.is_null()) {
            out = false;
            return;
        }
        if (v.kind() == ColumnType::boolean) {
            out = v.as_bool();
            return;
        }

        // Only 0 and 1, though. A TINYINT(1) holds anything from -128 to 127
        // and a SQLite INTEGER anything at all, and reading 5 as true hands the
        // next save a 1 to write over it -- a change to the data that nobody
        // made. Refused, so the column can be mapped as the integer it holds.
        const std::int64_t number = v.as_int64();
        if (number != 0 && number != 1) {
            throw conversion_error(std::format(
                "schemata: {} came back for a bool, which holds only 0 and 1; reading it "
                "as true would write 1 over it on the next save. Map the column as an "
                "integer if it holds other numbers",
                number));
        }
        out = number == 1;
    }
};

template <>
struct ValueConverter<std::int32_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::int32; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(std::int32_t v) { return Value{static_cast<std::int64_t>(v)}; }
    static void  from_value(const Value& v, std::int32_t& out) {
        out = v.is_null() ? 0 : static_cast<std::int32_t>(v.as_int64());
    }
};

template <>
struct ValueConverter<std::int64_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::int64; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value                to_value(std::int64_t v) { return Value{v}; }
    static void                 from_value(const Value& v, std::int64_t& out) {
        out = v.is_null() ? 0 : v.as_int64();
    }
};

template <>
struct ValueConverter<std::int8_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::int8; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(std::int8_t v) { return Value{static_cast<std::int64_t>(v)}; }
    static void  from_value(const Value& v, std::int8_t& out) {
        out = v.is_null() ? 0 : static_cast<std::int8_t>(v.as_int64());
    }
};

template <>
struct ValueConverter<std::int16_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::int16; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(std::int16_t v) { return Value{static_cast<std::int64_t>(v)}; }
    static void  from_value(const Value& v, std::int16_t& out) {
        out = v.is_null() ? 0 : static_cast<std::int16_t>(v.as_int64());
    }
};

template <>
struct ValueConverter<std::uint8_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::uint8; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(std::uint8_t v) { return Value{static_cast<std::int64_t>(v)}; }
    static void  from_value(const Value& v, std::uint8_t& out) {
        out = v.is_null() ? 0 : static_cast<std::uint8_t>(v.as_int64());
    }
};

template <>
struct ValueConverter<std::uint16_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::uint16; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(std::uint16_t v) { return Value{static_cast<std::int64_t>(v)}; }
    static void  from_value(const Value& v, std::uint16_t& out) {
        out = v.is_null() ? 0 : static_cast<std::uint16_t>(v.as_int64());
    }
};

template <>
struct ValueConverter<std::uint32_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::uint32; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(std::uint32_t v) { return Value{static_cast<std::int64_t>(v)}; }
    static void  from_value(const Value& v, std::uint32_t& out) {
        out = v.is_null() ? 0 : static_cast<std::uint32_t>(v.as_int64());
    }
};

/// The one integer type C++ has and the storage does not.
///
/// Carried as a `std::uint64_t` all the way to the driver, which binds it as
/// unsigned where the engine has somewhere to put it. Values above 2^63 are
/// representable on the MySQL family and are not on SQLite, where INTEGER is
/// signed 64-bit and there is no wider one -- so the SQLite driver refuses
/// such a value rather than storing a wrapped bit pattern.
template <>
struct ValueConverter<std::uint64_t> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::uint64; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(std::uint64_t v) { return Value{v}; }
    static void  from_value(const Value& v, std::uint64_t& out) {
        out = v.is_null() ? 0 : v.as_uint64();
    }
};

template <>
struct ValueConverter<float> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::real32; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(float v) { return Value{static_cast<double>(v)}; }
    static void  from_value(const Value& v, float& out) {
        out = v.is_null() ? 0.0f : static_cast<float>(v.as_real());
    }
};

template <>
struct ValueConverter<double> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::real; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value                to_value(double v) { return Value{v}; }
    static void                 from_value(const Value& v, double& out) {
        out = v.is_null() ? 0.0 : v.as_real();
    }
};

template <>
struct ValueConverter<std::string> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::text; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value                to_value(const std::string& v) { return Value{v}; }
    static void                 from_value(const Value& v, std::string& out) {
        out = v.is_null() ? std::string{} : v.as_text();
    }
};

template <>
struct ValueConverter<Blob> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::blob; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value                to_value(const Blob& v) { return Value{v}; }
    static void                 from_value(const Value& v, Blob& out) {
        out = v.is_null() ? Blob{} : v.as_blob();
    }
};

template <>
struct ValueConverter<timestamp> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::timestamp; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(timestamp v) { return Value{format_timestamp(v)}; }
    static void  from_value(const Value& v, timestamp& out) {
        out = v.is_null() ? timestamp{} : parse_timestamp(v.as_text());
    }
};

template <>
struct ValueConverter<date> {
    static constexpr ColumnType column_type() noexcept { return ColumnType::date; }
    static constexpr bool       nullable() noexcept { return false; }
    static Value to_value(date v) { return Value{format_date(v)}; }
    static void  from_value(const Value& v, date& out) {
        out = v.is_null() ? date{} : parse_date(v.as_text());
    }
};

// ---------------------------------------------------------------------------
// Composed
// ---------------------------------------------------------------------------

/// `std::optional<T>` is how a column is made nullable.
///
/// Nullability is a property of the C++ type rather than something declared
/// separately in `describe()`, so the two can never disagree.
template <typename T>
struct ValueConverter<std::optional<T>> {
    static constexpr ColumnType column_type() noexcept {
        return ValueConverter<T>::column_type();
    }
    static constexpr bool nullable() noexcept { return true; }

    static Value to_value(const std::optional<T>& v) {
        return v ? ValueConverter<T>::to_value(*v) : Value{};
    }

    static void from_value(const Value& v, std::optional<T>& out) {
        if (v.is_null()) {
            out.reset();
            return;
        }
        T inner{};
        ValueConverter<T>::from_value(v, inner);
        out = std::move(inner);
    }
};

/// Any enum stores as its underlying integer without further ceremony.
///
/// Specialise `ValueConverter` for a particular enum to store it as text
/// instead; an explicit specialisation is more specialised than this
/// constrained partial one and wins.
template <typename E>
struct ValueConverter<E, std::enable_if_t<std::is_enum_v<E>>> {
    using U = std::underlying_type_t<E>;

    static constexpr ColumnType column_type() noexcept {
        return sizeof(U) > 4 ? ColumnType::int64 : ColumnType::int32;
    }
    static constexpr bool nullable() noexcept { return false; }

    static Value to_value(E v) { return Value{static_cast<std::int64_t>(v)}; }

    static void from_value(const Value& v, E& out) {
        out = v.is_null() ? E{} : static_cast<E>(static_cast<U>(v.as_int64()));
    }
};

/// Satisfied when `T` can be stored in a column.
template <typename T>
concept Storable = requires(const T& value, Value v, T& out) {
    { ValueConverter<T>::column_type() } -> std::same_as<ColumnType>;
    { ValueConverter<T>::nullable() } -> std::same_as<bool>;
    { ValueConverter<T>::to_value(value) } -> std::same_as<Value>;
    ValueConverter<T>::from_value(v, out);
};

} // namespace schemata
