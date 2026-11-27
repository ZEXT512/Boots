#pragma once
#include <variant>
#include <string>
#include <utility>

/**
 * Simple Result<T> type similar to std::expected (C++23) for error handling.
 * Usage:
 *   auto r = SomeOperation();
 *   if (!r.is_ok()) return Result<void>::Err(r.error());
 */

template<typename T>
class Result {
public:
    // Construct success value
    static Result<T> Ok(T value) { return Result(std::move(value), std::monostate{}); }
    // Construct error
    static Result<T> Err(std::string msg) { return Result(std::monostate{}, std::move(msg)); }

    bool is_ok() const { return std::holds_alternative<T>(_value); }
    bool is_err() const { return std::holds_alternative<std::string>(_error); }

    T& unwrap() { return std::get<T>(_value); }
    const T& unwrap() const { return std::get<T>(_value); }
    const std::string& error() const { return std::get<std::string>(_error); }

private:
    // Store either value or monostate for void specialization
    std::variant<T, std::monostate> _value;
    std::variant<std::monostate, std::string> _error;
    Result(T val, std::monostate) : _value(std::move(val)), _error(std::monostate{}) {}
    Result(std::monostate, std::string err) : _value(std::monostate{}), _error(std::move(err)) {}
};

// Void specialization
template<>
class Result<void> {
public:
    static Result<void> Ok() { return Result(); }
    static Result<void> Err(std::string msg) { return Result(std::move(msg)); }
    bool is_ok() const { return _error.empty(); }
    bool is_err() const { return !_error.empty(); }
    const std::string& error() const { return _error; }
private:
    explicit Result() = default;
    explicit Result(std::string err) : _error(std::move(err)) {}
    std::string _error;
};
