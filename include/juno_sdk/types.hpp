/** @file types.hpp
 *  @brief Defines types used in the Juno inference model.
 */

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace juno::sdk {

/** Represents the role of a message in a conversation. */
enum class Role { System, User, Assistant, Tool };

/** Requested amount of model reasoning before producing a response. */
enum class ReasoningEffort { None, Low, Medium, High };

/** Represents a call to a tool during inference. */
struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments_json{"{}"};
};

/** Represents a message in a conversation. */
struct Message {
  Role role;
  std::string content;
  std::vector<ToolCall> tool_calls;
  std::string tool_call_id;
  std::string tool_name;
};

/** Options controlling model-facing conversation compaction. */
struct CompactionOptions {
  /** Absolute rendered-input threshold; takes precedence when nonzero. */
  std::size_t max_context_tokens{0};
  /** Percentage of the model runtime context used as the rendered-input threshold. */
  std::optional<float> max_context_percent{75.0F};
  /** Number of recent user turns to retain verbatim when compacting. */
  std::size_t preserve_recent_turns{4};
  /** Optional guidance recorded with the compacted summary. */
  std::string focus;
};

/** Model-facing context usage for a conversation. */
struct ContextUsage {
  /** Tokens in the complete rendered model prompt. */
  std::size_t input_tokens{0};
  /** Rendered input size that triggers automatic compaction. */
  std::size_t compaction_threshold{0};
  /** Tokens reserved for the next model response. */
  std::size_t output_reservation{0};
  /** Hard model context-window capacity. */
  std::size_t context_capacity{0};
  std::size_t message_count{0};
  /** False when the model backend can provide only an approximate count. */
  bool exact{false};
};

/** Metadata describing one compaction pass. */
struct CompactionResult {
  bool compacted{false};
  std::size_t messages_before{0};
  std::size_t messages_after{0};
  std::size_t input_tokens_before{0};
  std::size_t input_tokens_after{0};
};

/** Represents the definition of a tool that can be used during inference. */
struct ToolDefinition {
  std::string name;
  std::string description;
  std::string parameters_json{"{\"type\":\"object\"}"};
};

/** Configuration for text generation during inference. */
struct GenerationConfig {
  std::size_t max_tokens{512};
  float temperature{0.7F};
  float top_p{0.95F};
  unsigned int seed{0};
};

/** Represents the type of an event during inference. */
enum class EventType {
  Prompt,
  ReasoningDelta,
  TextDelta,
  ToolStarted,
  ToolCompleted,
  CompactionStarted,
  CompactionCompleted,
  Completed,
  Error
};

/** Represents an event during inference. */
struct AgentEvent {
  EventType type;
  std::string text;
  ToolCall tool_call;
};

/** Callback type for handling events during inference. */
using EventCallback = std::function<void(const AgentEvent &)>;

/** Represents the result of a run in the Juno inference model. */
struct RunResult {
  std::string final_text;
  std::vector<Message> messages;
  std::size_t inference_turns{0};
};

} // namespace juno::sdk
