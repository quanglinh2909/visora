#pragma once

// Result<T> — a value or an Error, with the failure reason attached.
//
// Replaces the `return false;` + `fprintf(stderr, ...)` idiom: a caller three
// layers up can still say WHY something failed instead of the reason being
// printed once and lost.

#include <string>
#include <utility>
#include <variant>

namespace visora::core {

enum class ErrorCode {
    Unsupported,       // this backend cannot do it; try another
    InvalidArgument,   // caller passed something nonsensical
    NotFound,          // a model, device or file is missing
    HardwareFailure,   // the device rejected or failed the operation
    Internal,          // a bug on our side
};

const char* toString(ErrorCode code);

struct Error {
    ErrorCode code = ErrorCode::Internal;
    std::string message;

    Error() = default;
    Error(ErrorCode c, std::string m) : code(c), message(std::move(m)) {}

    // Human-readable "unsupported: NV12 -> GRAY8 not implemented".
    std::string str() const;
};

inline Error unsupported(std::string m)     { return {ErrorCode::Unsupported, std::move(m)}; }
inline Error invalidArgument(std::string m) { return {ErrorCode::InvalidArgument, std::move(m)}; }
inline Error notFound(std::string m)        { return {ErrorCode::NotFound, std::move(m)}; }
inline Error hardwareFailure(std::string m) { return {ErrorCode::HardwareFailure, std::move(m)}; }
inline Error internalError(std::string m)   { return {ErrorCode::Internal, std::move(m)}; }

template <class T>
class Result {
public:
    Result(T value) : m_state(std::move(value)) {}          // NOLINT(google-explicit-constructor)
    Result(Error error) : m_state(std::move(error)) {}      // NOLINT(google-explicit-constructor)

    bool ok() const { return m_state.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() { return std::get<0>(m_state); }
    const T& value() const { return std::get<0>(m_state); }

    T valueOr(T fallback) const { return ok() ? std::get<0>(m_state) : std::move(fallback); }

    const Error& error() const { return std::get<1>(m_state); }

private:
    std::variant<T, Error> m_state;
};

// Result<void> — "it worked, or here is why it did not".
template <>
class Result<void> {
public:
    Result() = default;
    Result(Error error) : m_error(std::move(error)), m_ok(false) {}  // NOLINT(google-explicit-constructor)

    bool ok() const { return m_ok; }
    explicit operator bool() const { return m_ok; }

    const Error& error() const { return m_error; }

private:
    Error m_error;
    bool m_ok = true;
};

using Status = Result<void>;

}  // namespace visora::core
