/**
 * @file expected.hpp
 * @brief Defines the Expected type and related error handling types. Expected<T> = T or Error
 * Essentially Expected<T> is a std::variant<T, Error> with some convenience methods.
 */

#pragma once

#include <concepts>
#include <string>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace juno::sdk {

/** Error codes for the Juno inference model. */
enum class ErrorCode {
  InvalidConfiguration,
  ModelUnavailable,
  ModelLoadFailed,
  GenerationFailed,
  InvalidModelOutput,
  ToolExecutionFailed,
  ContextLimitExceeded,
  IterationLimitExceeded,
  Cancelled,
  SessionStorageFailed,
};

/** Represents an error in the Juno inference model. */
struct Error {
  ErrorCode code;
  std::string message;
};

/** Base exception for failures at the SDK boundary. */
class SdkError : public std::runtime_error {
public:
  explicit SdkError(Error error)
      : std::runtime_error(error.message), code(error.code), message(std::move(error.message)) {}

  ErrorCode code;
  std::string message;
};

/** Thrown when user-supplied SDK options are invalid. */
class ConfigurationError : public SdkError {
public:
  using SdkError::SdkError;
};

/** Thrown when an inference model cannot be loaded or used. */
class ModelError : public SdkError {
public:
  using SdkError::SdkError;
};

template <typename T>
class Expected {
 public:
  Expected(T value) : value_(std::move(value)) {}
  template <typename U>
    requires(!std::same_as<std::remove_cvref_t<U>, Expected> && std::constructible_from<T, U&&>)
  Expected(U&& value) : value_(T(std::forward<U>(value))) {}
  Expected(Error error) : value_(std::move(error)) {}

  [[nodiscard]] bool has_value() const { return std::holds_alternative<T>(value_); }
  [[nodiscard]] explicit operator bool() const { return has_value(); }
  [[nodiscard]] T& value() { return std::get<T>(value_); }
  [[nodiscard]] const T& value() const { return std::get<T>(value_); }
  [[nodiscard]] Error& error() { return std::get<Error>(value_); }
  [[nodiscard]] const Error& error() const { return std::get<Error>(value_); }

 private:
  std::variant<T, Error> value_;
};

template <>
class Expected<void> {
 public:
  Expected() : error_(std::monostate{}) {}
  Expected(Error error) : error_(std::move(error)) {}

  [[nodiscard]] bool has_value() const { return std::holds_alternative<std::monostate>(error_); }
  [[nodiscard]] explicit operator bool() const { return has_value(); }
  [[nodiscard]] Error& error() { return std::get<Error>(error_); }
  [[nodiscard]] const Error& error() const { return std::get<Error>(error_); }

 private:
  std::variant<std::monostate, Error> error_;
};

inline Error make_unexpected(Error error) {
  return error;
}

}  // namespace juno::sdk
