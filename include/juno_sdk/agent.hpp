/**
 * @file agent.hpp
 * @brief Defines the Agent and Conversation classes for managing conversations
 * with a model.
 */

#pragma once

#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "juno_sdk/memory.hpp"
#include "juno_sdk/model.hpp"

namespace juno::sdk {

using JsonObject = nlohmann::json;

using ToolHandler = std::function<Expected<std::string>(std::string_view arguments_json)>;

/** The result returned by a readable JSON-based tool handler. */
struct ToolResult {
  bool success{true};
  std::string content;
  JsonObject data{JsonObject::object()};
};

using JsonToolHandler = std::function<ToolResult(const JsonObject &params)>;

/** A parameter shown to the model when a tool is registered. */
struct ToolParameter {
  std::string name;
  std::string description;
  std::string type;
  bool required{false};
};

struct ToolOptions {
  std::string name;
  std::string description;
  std::vector<ToolParameter> parameters;
  JsonToolHandler handler;
};

/** A user-authored document that guides agent behavior. */
struct SteeringDocument {
  std::string name;
  std::string content;
};

/** Steering documents loaded into the initial system context. */
struct SteeringOptions {
  std::vector<SteeringDocument> documents;
  std::size_t max_bytes{32 * 1024};
};

/** A tool definition and the handler that executes it. */
struct Tool {
  ToolDefinition definition;
  ToolHandler handler;
  JsonToolHandler json_handler;

  /** Creates and validates a tool definition. */
  static Tool create(ToolOptions options);
};

/** Immutable behavior shared by every conversation created from an Agent. */
struct AgentOptions {
  std::shared_ptr<Model> model;
  std::string system_prompt;
  SteeringOptions steering;
  GenerationConfig generation;
  ReasoningEffort reasoning_effort{ReasoningEffort::Medium};
  std::size_t max_inference_turns{8};
  CompactionOptions compaction;
  std::vector<Tool> tools;
  std::shared_ptr<MemoryManager> memory;
};

/** One independent conversation with an Agent. */
class Conversation {
public:
  // Generates a response to the given user message, invoking the callback for
  // events.
  [[nodiscard]] Expected<RunResult>
  run(std::string_view user_message, EventCallback callback = {}, std::stop_token stop_token = {});

  /** Compacts the model-facing context while retaining the full transcript. */
  [[nodiscard]] Expected<CompactionResult> compact(
      CompactionOptions options = {},
      EventCallback callback = {},
      std::stop_token stop_token = {}
  );

  // Returns the history of messages in this conversation.
  [[nodiscard]] const std::vector<Message> &history() const;
  /** Returns rendered model-facing context usage before temporary memory injection. */
  [[nodiscard]] Expected<ContextUsage> context_usage() const;
  void clear();

private:
  friend class Agent;
  Conversation(
      std::shared_ptr<Model> model,
      std::shared_ptr<const AgentOptions> options,
      std::shared_ptr<MemoryManager> memory
  );

  std::shared_ptr<Model> model_;
  std::shared_ptr<const AgentOptions> options_;
  std::shared_ptr<MemoryManager> memory_;
  std::vector<Message> history_;
  std::vector<Message> context_;
};

/** Reusable model and immutable behavior used to create conversations. */
class Agent {
public:
  /** Creates and validates a reusable agent. */
  static Agent create(AgentOptions options);

  Agent(Agent &&) = default;
  Agent &operator=(Agent &&) = default;
  Agent(const Agent &) = default;
  Agent &operator=(const Agent &) = default;

  [[nodiscard]] Conversation start_conversation() const;
  /** Adds a tool for conversations created after this call. */
  void add_tool(Tool tool);
  void set_memory(std::shared_ptr<MemoryManager> memory);

private:
  Agent(AgentOptions options);
  std::shared_ptr<Model> model_;
  AgentOptions options_;
  std::shared_ptr<MemoryManager> memory_;
};

} // namespace juno::sdk
