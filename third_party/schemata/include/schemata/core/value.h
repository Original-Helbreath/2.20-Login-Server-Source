#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <schemata/core/types.h>

namespace schemata {

/// One column value, in transit between an entity field and a statement.
///
/// Six alternatives, not eight: `timestamp` and `date` are carried as text
/// rather than as their own kinds. That is possible because a single
/// rendering works everywhere -- `YYYY-MM-DD HH:MM:SS.ffffff` is what MySQL
/// and MariaDB parse into `DATETIME(6)`, and it is also what SQLite should
/// store as `TEXT` under D7, where it has the useful property of sorting
/// correctly as a string. One format, no dialect parameter, no per-driver
/// date handling.
class Value {
public:
    /// Nothing reads these by index -- `kind()` and the binder both go
    /// through `std::visit` -- so the order is free to say what it means.
    using Storage = std::variant<std::monostate,  // null
                                 bool,
                                 std::int64_t,
                                 std::uint64_t,
                                 double,
                                 std::string,
                                 Blob>;

    Value() noexcept = default;                       ///< null
    Value(std::nullptr_t) noexcept {}                 ///< null, explicitly
    Value(bool v) noexcept : storage_(v) {}
    Value(std::int64_t v) noexcept : storage_(v) {}

    /// Its own alternative, because the top of its range is not a signed
    /// 64-bit value at all. Folding it into `int64_t` would make a large
    /// `uint64` indistinguishable from a negative number the moment it was
    /// stored, and the drivers could no longer tell the server which it is.
    Value(std::uint64_t v) noexcept : storage_(v) {}
    Value(double v) noexcept : storage_(v) {}
    Value(std::string v) : storage_(std::move(v)) {}
    Value(std::string_view v) : storage_(std::string(v)) {}
    Value(const char* v) : storage_(std::string(v)) {}
    Value(Blob v) : storage_(std::move(v)) {}

    [[nodiscard]] bool is_null() const noexcept {
        return std::holds_alternative<std::monostate>(storage_);
    }

    [[nodiscard]] const Storage& storage() const noexcept { return storage_; }

    /// Which alternative is held, as a ColumnType. Meaningless when null.
    [[nodiscard]] ColumnType kind() const;

    // Accessors. Each throws std::bad_variant_access on a mismatch, which is
    // a programmer error: the model says what a column holds.
    [[nodiscard]] bool               as_bool() const { return std::get<bool>(storage_); }
    [[nodiscard]] std::int64_t       as_int64() const { return std::get<std::int64_t>(storage_); }

    /// Reads either integer alternative.
    ///
    /// A `uint64` column read back off the wire arrives as a signed 64-bit
    /// value whose bit pattern is already right, so reinterpreting it is the
    /// correct reading rather than a workaround -- there is nowhere else for
    /// the top of the range to have gone.
    [[nodiscard]] std::uint64_t as_uint64() const {
        if (std::holds_alternative<std::int64_t>(storage_)) {
            return static_cast<std::uint64_t>(std::get<std::int64_t>(storage_));
        }
        return std::get<std::uint64_t>(storage_);
    }
    [[nodiscard]] double             as_real() const { return std::get<double>(storage_); }
    [[nodiscard]] const std::string& as_text() const { return std::get<std::string>(storage_); }
    [[nodiscard]] const Blob&        as_blob() const { return std::get<Blob>(storage_); }

    // Move-outs, for a caller done with the value: `std::move(v).take_text()`.
    // Each moves what is held rather than copying it -- a string's or a
    // blob's buffer goes to the caller as it is -- and leaves this value
    // null. `take_text()` and `take_blob()` throw std::bad_variant_access on
    // a mismatch, as `as_text()` and `as_blob()` do, and then leave the value
    // as it was.

    /// Whatever is held, as its alternative.
    [[nodiscard]] Storage take() && noexcept {
        Storage out = std::move(storage_);
        storage_.emplace<std::monostate>();
        return out;
    }

    [[nodiscard]] std::string take_text() && {
        std::string out = std::move(std::get<std::string>(storage_));
        storage_.emplace<std::monostate>();
        return out;
    }

    [[nodiscard]] Blob take_blob() && {
        Blob out = std::move(std::get<Blob>(storage_));
        storage_.emplace<std::monostate>();
        return out;
    }

    /// Equality is by alternative and content, so a null never equals a
    /// zero, an empty string or an empty blob. This is what the change
    /// tracker's dirty check rests on.
    [[nodiscard]] bool operator==(const Value&) const = default;

private:
    Storage storage_;
};

// ---------------------------------------------------------------------------
// Datetime rendering
//
// Shared by both drivers rather than reimplemented in each. Hand-rolled
// rather than routed through std::chrono::parse, whose support is uneven
// across the standard libraries this targets and whose flexibility is not
// wanted here: the format is fixed, and anything else is a defect.
// ---------------------------------------------------------------------------

/// Render as `YYYY-MM-DD HH:MM:SS.ffffff`, always UTC, always six digits.
[[nodiscard]] std::string format_timestamp(timestamp value);

/// Render as `YYYY-MM-DD`.
[[nodiscard]] std::string format_date(date value);

/// Parse `YYYY-MM-DD HH:MM:SS[.ffffff]`.
///
/// Accepts a `T` separator as well as a space, and a fractional part of any
/// length or none, because that is the range of what the three engines
/// actually hand back.
///
/// \throws conversion_error when the text is not a timestamp.
[[nodiscard]] timestamp parse_timestamp(std::string_view text);

/// Parse `YYYY-MM-DD`, ignoring any time part that follows.
///
/// \throws conversion_error when the text is not a date.
[[nodiscard]] date parse_date(std::string_view text);

} // namespace schemata
