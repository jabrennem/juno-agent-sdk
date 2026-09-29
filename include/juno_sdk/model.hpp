/** @file model.hpp
 *  @brief Defines the interface for the Juno inference model.
 */

#pragma once

#include "juno_sdk/expected.hpp"
#include "juno_sdk/types.hpp"
#include <span>
#include <stop_token>

/** Namespace for the Juno inference model. */
namespace juno::sdk {

/** Request for generating text. */
struct GenerationRequest {
  std::span<const Message> messages;
  std::span<const ToolDefinition> tools;
  GenerationConfig config;
  ReasoningEffort reasoning_effort{ReasoningEffort::Medium};
};

/** Response for a generation request. */
struct GenerationResponse {
  std::string content;
  std::vector<ToolCall> tool_calls;
};

/** Token usage for a complete model-facing generation request. */
struct PromptMetrics {
  std::size_t input_tokens{0};
  /** False when the backend uses a conservative estimate instead of its tokenizer. */
  bool exact{false};
};

/** Base class for all inference models. */
class Model {
public:
  virtual ~Model() = default;
  /** Returns the configured model context size, or zero when unknown. */
  virtual std::size_t context_size() const {
    return 0;
  }
  /** Measures the complete rendered prompt, including tool and template overhead. */
  virtual Expected<PromptMetrics> measure(const GenerationRequest &request);
  virtual Expected<GenerationResponse> generate(
      const GenerationRequest &request,
      const EventCallback &callback,
      std::stop_token stop_token
  ) = 0;
};

} // namespace juno::sdk
