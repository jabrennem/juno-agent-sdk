/**
 * @file agent.cpp
 * @brief Implements the Agent and Conversation inference loop.
 */

#include "juno_sdk/agent.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace juno::sdk {
namespace {

std::string parameter_schema(const std::vector<ToolParameter> &parameters) {
  JsonObject schema{{"type", "object"}, {"properties", JsonObject::object()}};
  auto &properties = schema["properties"];
  JsonObject required = JsonObject::array();
  for (const auto &parameter : parameters) {
    properties[parameter.name] = {{"type", parameter.type}, {"description", parameter.description}};
    if (parameter.required)
      required.push_back(parameter.name);
  }
  if (!required.empty())
    schema["required"] = std::move(required);
  return schema.dump();
}

std::string tool_error_json(const std::string_view message) {
  std::string escaped;
  escaped.reserve(message.size());
  for (const char character : message) {
    if (character == '"' || character == '\\')
      escaped.push_back('\\');
    escaped.push_back(character);
  }
  return "{\"error\":\"" + escaped + "\"}";
}

void emit(const EventCallback &callback, AgentEvent event) {
  if (callback)
    callback(event);
}

std::size_t estimated_message_tokens(const std::vector<Message> &messages) {
  std::size_t characters = 0;
  for (const auto &message : messages) {
    characters += message.content.size();
    for (const auto &call : message.tool_calls)
      characters += call.name.size() + call.arguments_json.size();
  }
  return (characters + 3) / 4;
}

std::size_t configured_compaction_threshold(const CompactionOptions &options, const Model &model) {
  if (options.max_context_tokens > 0)
    return options.max_context_tokens;
  if (!options.max_context_percent || model.context_size() == 0)
    return 0;
  return static_cast<std::size_t>(
      static_cast<float>(model.context_size()) * *options.max_context_percent / 100.0F
  );
}

std::size_t effective_compaction_threshold(
    const CompactionOptions &options,
    const Model &model,
    std::size_t output_reservation
) {
  const auto configured = configured_compaction_threshold(options, model);
  const auto capacity = model.context_size();
  if (configured == 0 || capacity == 0)
    return configured;
  const auto safe_input_limit =
      output_reservation < capacity ? capacity - output_reservation : std::size_t{0};
  return std::min(configured, safe_input_limit);
}

std::vector<ToolDefinition> tool_definitions(const AgentOptions &options) {
  std::vector<ToolDefinition> definitions;
  definitions.reserve(options.tools.size());
  for (const auto &tool : options.tools)
    definitions.push_back(tool.definition);
  return definitions;
}

std::vector<Message> model_history(const std::vector<Message> &context) {
  return context;
}

std::vector<Message> prompt_measurement_history(const std::vector<Message> &context) {
  std::vector<Message> result = context;
  const bool has_user_message =
      std::any_of(result.begin(), result.end(), [](const Message &message) {
        return message.role == Role::User;
      });
  if (!has_user_message)
    result.push_back(Message{Role::User, {}});
  return result;
}

Error context_limit_error(
    std::size_t input_tokens,
    std::size_t output_reservation,
    std::size_t capacity
) {
  return Error{
      ErrorCode::ContextLimitExceeded,
      "rendered prompt requires " + std::to_string(input_tokens) + " input tokens + "
          + std::to_string(output_reservation) + " output tokens, exceeding the "
          + std::to_string(capacity) + "-token context capacity"
  };
}

std::string role_name(Role role) {
  switch (role) {
  case Role::System:
    return "system";
  case Role::User:
    return "user";
  case Role::Assistant:
    return "assistant";
  case Role::Tool:
    return "tool";
  }
  return "unknown";
}

std::string conversation_transcript(
    const std::vector<Message> &messages,
    std::size_t begin,
    std::size_t end,
    std::string_view focus
) {
  std::ostringstream summary;
  if (!focus.empty())
    summary << "Compaction focus: " << focus << "\n\n";
  for (std::size_t index = begin; index < end; ++index) {
    const auto &message = messages[index];
    summary << "[" << role_name(message.role) << "] " << message.content;
    if (!message.tool_calls.empty()) {
      summary << " (tool calls:";
      for (const auto &call : message.tool_calls)
        summary << " " << call.name;
      summary << ")";
    }
    summary << "\n";
  }
  return summary.str();
}

std::size_t
recent_turn_start(const std::vector<Message> &messages, std::size_t preserve_recent_turns) {
  if (preserve_recent_turns == 0)
    return messages.size();
  std::size_t user_turns = 0;
  for (std::size_t index = messages.size(); index > 0; --index) {
    if (messages[index - 1].role == Role::User && ++user_turns == preserve_recent_turns)
      return index - 1;
  }
  return 0;
}

std::string memory_store_parameter_description(const MemoryManager &memory) {
  std::string description = "Optional target store. Available stores:";
  for (const auto &store : memory.stores()) {
    description += "\n- " + store->name();
    if (!store->description().empty())
      description += ": " + store->description();
  }
  return description;
}

std::string initial_system_context(const AgentOptions &options) {
  std::string context = options.system_prompt;
  if (options.steering && !options.steering->system_context().empty()) {
    if (!context.empty())
      context += "\n\n";
    context += options.steering->system_context();
  }
  return context;
}

} // namespace

Expected<PromptMetrics> Model::measure(const GenerationRequest &request) {
  std::size_t characters = 0;
  for (const auto &message : request.messages) {
    characters += message.content.size();
    characters += message.tool_call_id.size() + message.tool_name.size();
    for (const auto &call : message.tool_calls)
      characters += call.id.size() + call.name.size() + call.arguments_json.size();
  }
  for (const auto &tool : request.tools)
    characters += tool.name.size() + tool.description.size() + tool.parameters_json.size();
  return PromptMetrics{(characters + 3) / 4, false};
}

std::shared_ptr<const Steering> Steering::create(SteeringOptions options) {
  if (options.max_bytes == 0)
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "steering.max_bytes must be greater than zero"}
    );

  for (const auto &path : options.files) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
      throw ConfigurationError(
          Error{ErrorCode::InvalidConfiguration, "could not open steering file: " + path}
      );

    std::ostringstream content;
    content << input.rdbuf();
    if (input.bad())
      throw ConfigurationError(
          Error{ErrorCode::InvalidConfiguration, "could not read steering file: " + path}
      );
    options.documents.push_back({.name = path, .content = content.str()});
  }

  std::size_t steering_bytes = 0;
  std::unordered_set<std::string> steering_names;
  for (const auto &document : options.documents) {
    if (document.name.empty())
      throw ConfigurationError(
          Error{ErrorCode::InvalidConfiguration, "steering document name is required"}
      );
    if (!steering_names.insert(document.name).second)
      throw ConfigurationError(
          Error{
              ErrorCode::InvalidConfiguration,
              "steering document names must be unique: " + document.name
          }
      );
    if (document.content.size() > options.max_bytes - std::min(steering_bytes, options.max_bytes))
      throw ConfigurationError(
          Error{
              ErrorCode::InvalidConfiguration, "steering documents exceed the configured byte limit"
          }
      );
    steering_bytes += document.content.size();
  }

  return std::shared_ptr<const Steering>(new Steering(std::move(options)));
}

Steering::Steering(SteeringOptions options) : options_(std::move(options)) {
  for (const auto &document : options_.documents) {
    if (!system_context_.empty())
      system_context_ += "\n\n";
    system_context_ += "Steering document: " + document.name + "\n" + document.content;
  }
}

const std::vector<SteeringDocument> &Steering::documents() const {
  return options_.documents;
}

const std::string &Steering::system_context() const {
  return system_context_;
}

Tool Tool::create(ToolOptions options) {
  if (options.name.empty())
    throw ConfigurationError(Error{ErrorCode::InvalidConfiguration, "tool name is required"});
  if (options.description.empty())
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "tool description is required: " + options.name}
    );
  if (!options.handler)
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "tool handler is required: " + options.name}
    );
  std::unordered_set<std::string> names;
  for (const auto &parameter : options.parameters) {
    if (parameter.name.empty() || parameter.type.empty())
      throw ConfigurationError(
          Error{
              ErrorCode::InvalidConfiguration,
              "tool parameters require a name and type: " + options.name
          }
      );
    if (!names.insert(parameter.name).second)
      throw ConfigurationError(
          Error{
              ErrorCode::InvalidConfiguration,
              "tool parameter names must be unique: " + parameter.name
          }
      );
  }
  return Tool{
      ToolDefinition{
          std::move(options.name),
          std::move(options.description),
          parameter_schema(options.parameters)
      },
      {},
      std::move(options.handler)
  };
}

Agent Agent::create(AgentOptions options) {
  if (!options.model)
    throw ConfigurationError(Error{ErrorCode::InvalidConfiguration, "agent model is required"});
  if (options.max_inference_turns == 0)
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "max_inference_turns must be greater than zero"}
    );
  if (options.generation.max_tokens == 0)
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "generation.max_tokens must be greater than zero"}
    );
  if (options.generation.temperature < 0.0F || options.generation.temperature > 2.0F)
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "generation.temperature must be between 0 and 2"}
    );
  if (options.compaction.max_context_percent
      && (*options.compaction.max_context_percent < 0.0F
          || *options.compaction.max_context_percent > 100.0F))
    throw ConfigurationError(
        Error{
            ErrorCode::InvalidConfiguration,
            "compaction.max_context_percent must be between 0 and 100"
        }
    );
  std::unordered_set<std::string> names;
  for (const auto &tool : options.tools) {
    if (tool.definition.name.empty() || (!tool.handler && !tool.json_handler))
      throw ConfigurationError(
          Error{ErrorCode::InvalidConfiguration, "agent tools must have a name and handler"}
      );
    if (!names.insert(tool.definition.name).second)
      throw ConfigurationError(
          Error{
              ErrorCode::InvalidConfiguration,
              "agent tool names must be unique: " + tool.definition.name
          }
      );
  }
  return Agent(std::move(options));
}

Agent::Agent(AgentOptions options)
    : model_(options.model), options_(std::move(options)), memory_(options_.memory) {
  if (memory_)
    set_memory(memory_);
}

Conversation Agent::start_conversation() const {
  return Conversation{model_, std::make_shared<const AgentOptions>(options_), memory_};
}

std::vector<ToolDefinition> Agent::tool_definitions() const {
  // Return a snapshot without duplicating handlers or exposing mutable state.
  std::vector<ToolDefinition> definitions;
  definitions.reserve(options_.tools.size());
  for (const auto &tool : options_.tools)
    definitions.push_back(tool.definition);
  return definitions;
}

void Agent::add_tool(Tool tool) {
  if (tool.definition.name.empty() || (!tool.handler && !tool.json_handler))
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "agent tools must have a name and handler"}
    );
  const auto duplicate =
      std::any_of(options_.tools.begin(), options_.tools.end(), [&tool](const Tool &value) {
        return value.definition.name == tool.definition.name;
      });
  if (duplicate)
    throw ConfigurationError(
        Error{
            ErrorCode::InvalidConfiguration,
            "agent tool names must be unique: " + tool.definition.name
        }
    );
  options_.tools.push_back(std::move(tool));
}

void Agent::set_memory(std::shared_ptr<MemoryManager> memory) {
  memory_ = std::move(memory);
  options_.memory = memory_;
  if (memory_ && memory_->file_tools_enabled()) {
    const auto register_file_tool = [this](std::string name, std::string description,
                                           std::vector<ToolParameter> parameters,
                                           JsonToolHandler handler) {
      const bool already_registered = std::any_of(
          options_.tools.begin(), options_.tools.end(), [&name](const Tool &tool) {
            return tool.definition.name == name;
          });
      if (!already_registered)
        add_tool(Tool::create({.name = std::move(name),
                               .description = std::move(description),
                               .parameters = std::move(parameters),
                               .handler = std::move(handler)}));
    };
    const auto file_parameters = std::vector<ToolParameter>{
        {"path", "A Markdown filename in the memory directory", "string", true},
        {"store", memory_store_parameter_description(*memory_), "string", false}};
    register_file_tool(
        "read_memory_file", "Read a Markdown file from durable memory.", file_parameters,
        [memory = memory_](const JsonObject &params) -> ToolResult {
          if (!params.contains("path") || !params["path"].is_string())
            return {false, "read_memory_file requires a string path"};
          const auto result = memory->read_file(params["path"].get<std::string>(),
                                                params.value("store", ""));
          return result ? ToolResult{true, result.value()} : ToolResult{false, result.error().message};
        });
    register_file_tool(
        "write_memory_file", "Create or replace a Markdown file in durable memory.",
        {file_parameters[0], {"content", "Complete Markdown file contents", "string", true},
         file_parameters[1]},
        [memory = memory_](const JsonObject &params) -> ToolResult {
          if (!params.contains("path") || !params["path"].is_string()
              || !params.contains("content") || !params["content"].is_string())
            return {false, "write_memory_file requires string path and content"};
          const auto result = memory->write_file(params["path"].get<std::string>(),
                                                 params["content"].get<std::string>(),
                                                 params.value("store", ""));
          return result ? ToolResult{true, "Memory file written."}
                        : ToolResult{false, result.error().message};
        });
    register_file_tool(
        "rename_memory_file", "Rename a Markdown file within durable memory.",
        {{"from", "Existing Markdown filename", "string", true},
         {"to", "New Markdown filename", "string", true}, file_parameters[1]},
        [memory = memory_](const JsonObject &params) -> ToolResult {
          if (!params.contains("from") || !params["from"].is_string()
              || !params.contains("to") || !params["to"].is_string())
            return {false, "rename_memory_file requires string from and to"};
          const auto result = memory->rename_file(params["from"].get<std::string>(),
                                                  params["to"].get<std::string>(),
                                                  params.value("store", ""));
          return result ? ToolResult{true, "Memory file renamed."}
                        : ToolResult{false, result.error().message};
        });
    register_file_tool(
        "delete_memory_file", "Delete a Markdown file from durable memory.", file_parameters,
        [memory = memory_](const JsonObject &params) -> ToolResult {
          if (!params.contains("path") || !params["path"].is_string())
            return {false, "delete_memory_file requires a string path"};
          const auto result = memory->delete_file(params["path"].get<std::string>(),
                                                  params.value("store", ""));
          return result ? ToolResult{true, "Memory file deleted."}
                        : ToolResult{false, result.error().message};
        });
  }
}

Conversation::Conversation(
    std::shared_ptr<Model> model,
    std::shared_ptr<const AgentOptions> options,
    std::shared_ptr<MemoryManager> memory
)
    : model_(std::move(model)), options_(std::move(options)), memory_(std::move(memory)) {
  const auto system_context = initial_system_context(*options_);
  if (!system_context.empty()) {
    history_.push_back(Message{Role::System, system_context});
    context_.push_back(history_.back());
  }
}

/**
 * @brief Runs the conversation with the given user message and event callback.
 *
 * This method processes the user message, generates a response using the model,
 * and handles tool calls if any are present in the response. It emits events to
 * the provided callback during the process.
 *
 * @param user_message The message from the user to be processed.
 * @param callback The callback function to handle events during the run.
 * @param stop_token A token to request cancellation of the run.
 * @return Expected containing RunResult on success or an Error on failure.
 */
Expected<RunResult> Conversation::run(
    const std::string_view user_message,
    EventCallback callback,
    std::stop_token stop_token
) {
  if (!model_) {
    return make_unexpected(Error{ErrorCode::InvalidConfiguration, "agent has no model"});
  }
  if (stop_token.stop_requested()) {
    return make_unexpected(Error{ErrorCode::Cancelled, "agent run was cancelled"});
  }

  // Add the user's message to the conversation history.
  history_.push_back(Message{Role::User, std::string(user_message)});
  context_.push_back(history_.back());

  // Prepare the list of tool definitions for the generation request.
  const auto definitions = tool_definitions(*options_);

  // Run the inference loop for a maximum number of turns as specified in the
  // configuration.
  for (std::size_t turn = 1; turn <= options_->max_inference_turns; ++turn) {
    if (stop_token.stop_requested()) {
      emit(callback, AgentEvent{EventType::Error, "agent run was cancelled", {}});
      return make_unexpected(Error{ErrorCode::Cancelled, "agent run was cancelled"});
    }

    // Build and measure the complete model-facing request before deciding whether to compact.
    // This includes temporary memory and tool definitions, both of which affect rendered size.
    auto request_history = model_history(context_);
    GenerationRequest request{
        request_history, definitions, options_->generation, options_->reasoning_effort
    };
    auto metrics = model_->measure(request);
    if (!metrics) {
      emit(callback, AgentEvent{EventType::Error, metrics.error().message, {}});
      return make_unexpected(metrics.error());
    }

    const auto threshold = effective_compaction_threshold(
        options_->compaction, *model_, options_->generation.max_tokens
    );
    if (threshold > 0 && metrics.value().input_tokens >= threshold) {
      auto compacted = compact(options_->compaction, callback, stop_token);
      if (!compacted)
        return make_unexpected(compacted.error());

      // Compaction output is model-generated and has no guaranteed size. Rebuild and measure the
      // complete request instead of assuming the summary now fits.
      request_history = model_history(context_);
      request = GenerationRequest{
          request_history, definitions, options_->generation, options_->reasoning_effort
      };
      metrics = model_->measure(request);
      if (!metrics) {
        emit(callback, AgentEvent{EventType::Error, metrics.error().message, {}});
        return make_unexpected(metrics.error());
      }
    }

    const auto capacity = model_->context_size();
    if (capacity > 0
        && (metrics.value().input_tokens > capacity
            || options_->generation.max_tokens > capacity - metrics.value().input_tokens)) {
      const auto error = context_limit_error(
          metrics.value().input_tokens, options_->generation.max_tokens, capacity
      );
      emit(callback, AgentEvent{EventType::Error, error.message, {}});
      return make_unexpected(error);
    }

    auto response = model_->generate(request, callback, stop_token);
    if (!response) {
      emit(callback, AgentEvent{EventType::Error, response.error().message, {}});
      return make_unexpected(response.error());
    }

    // Add the assistant's response to the conversation history.
    history_.push_back(
        Message{Role::Assistant, response.value().content, response.value().tool_calls}
    );
    context_.push_back(history_.back());
    if (response.value().tool_calls.empty()) {
      RunResult result{response.value().content, history_, turn};
      emit(callback, AgentEvent{EventType::Completed, result.final_text, {}});
      return result;
    }

    // Process each tool call in the assistant's response.
    for (const ToolCall &call : response.value().tool_calls) {
      if (stop_token.stop_requested()) {
        emit(callback, AgentEvent{EventType::Error, "agent run was cancelled", call});
        return make_unexpected(Error{ErrorCode::Cancelled, "agent run was cancelled"});
      }
      emit(callback, AgentEvent{EventType::ToolStarted, {}, call});
      std::string output;
      const auto registered =
          std::find_if(options_->tools.begin(), options_->tools.end(), [&call](const Tool &tool) {
            return tool.definition.name == call.name;
          });
      if (registered == options_->tools.end()) {
        output = tool_error_json("unknown tool: " + call.name);
      } else if (!registered->handler && !registered->json_handler) {
        output = tool_error_json("tool has no handler: " + call.name);
      } else {
        try {
          if (registered->json_handler) {
            const auto params = JsonObject::parse(call.arguments_json);
            const auto result = registered->json_handler(params);
            output = result.success ? result.content : tool_error_json(result.content);
            if (output.empty() && !result.data.is_null())
              output = result.data.dump();
          } else {
            auto tool_result = registered->handler(call.arguments_json);
            output =
                tool_result ? tool_result.value() : tool_error_json(tool_result.error().message);
          }
        } catch (const std::exception &exception) {
          output = tool_error_json(std::string("tool threw: ") + exception.what());
        } catch (...) {
          output = tool_error_json("tool threw an unknown exception");
        }
      }
      // Add the tool's output to the conversation history and emit a
      // ToolCompleted event.
      history_.push_back(Message{Role::Tool, std::move(output), {}, call.id, call.name});
      context_.push_back(history_.back());
      emit(callback, AgentEvent{EventType::ToolCompleted, history_.back().content, call});
    }
  }

  // If the maximum number of inference turns is reached, emit an error event
  // and return an error.
  emit(callback, AgentEvent{EventType::Error, "agent reached its inference-turn limit", {}});
  return make_unexpected(
      Error{ErrorCode::IterationLimitExceeded, "agent reached its inference-turn limit"}
  );
}

const std::vector<Message> &Conversation::history() const {
  return history_;
}

Expected<ContextUsage> Conversation::context_usage() const {
  const auto definitions = tool_definitions(*options_);
  const auto measurement_history = prompt_measurement_history(context_);
  const GenerationRequest request{
      measurement_history, definitions, options_->generation, options_->reasoning_effort
  };
  auto metrics = model_->measure(request);
  if (!metrics)
    return make_unexpected(metrics.error());
  return ContextUsage{
      metrics.value().input_tokens,
      effective_compaction_threshold(
          options_->compaction, *model_, options_->generation.max_tokens
      ),
      options_->generation.max_tokens,
      model_->context_size(),
      context_.size(),
      metrics.value().exact
  };
}

Expected<CompactionResult> Conversation::compact(
    CompactionOptions options,
    EventCallback callback,
    std::stop_token stop_token
) {
  if (stop_token.stop_requested())
    return make_unexpected(Error{ErrorCode::Cancelled, "compaction was cancelled"});

  const auto definitions = tool_definitions(*options_);
  const auto before_history = prompt_measurement_history(context_);
  const GenerationRequest before_request{
      before_history, definitions, options_->generation, options_->reasoning_effort
  };
  auto before_metrics = model_->measure(before_request);
  if (!before_metrics) {
    emit(callback, AgentEvent{EventType::Error, before_metrics.error().message, {}});
    return make_unexpected(before_metrics.error());
  }
  CompactionResult result{
      false,
      context_.size(),
      context_.size(),
      before_metrics.value().input_tokens,
      before_metrics.value().input_tokens
  };
  const std::size_t system_count = !context_.empty() && context_.front().role == Role::System;
  const std::size_t first_conversation_message = system_count ? 1 : 0;
  if (context_.size() <= first_conversation_message + 1)
    return result;

  const std::vector<Message> conversation_messages(
      context_.begin() + first_conversation_message, context_.end()
  );
  const std::size_t recent_start =
      first_conversation_message
      + recent_turn_start(conversation_messages, options.preserve_recent_turns);
  if (recent_start <= first_conversation_message
      || (recent_start == first_conversation_message + conversation_messages.size()
          && options.preserve_recent_turns > 0))
    return result;

  emit(callback, AgentEvent{EventType::CompactionStarted, {}, {}});
  const std::vector<Message> older_messages(
      context_.begin() + first_conversation_message, context_.begin() + recent_start
  );
  const std::size_t older_tokens = estimated_message_tokens(older_messages);
  const std::string transcript =
      conversation_transcript(context_, first_conversation_message, recent_start, options.focus);
  const std::string summary_instruction =
      "Summarize the earlier conversation for an agent that will continue the task. "
      "Preserve decisions, user preferences, important facts, tool results, unresolved work, "
      "and constraints. Do not invent facts. The summary must be substantially shorter than "
      "the source transcript. Return only the concise summary.\n\n"
      + transcript;
  const std::vector<Message> summary_messages{
      Message{
          Role::System,
          "You are a conversation-compaction assistant. Your summary will be inserted "
          "into the agent's working context."
      },
      Message{Role::User, summary_instruction}
  };
  const std::vector<ToolDefinition> no_tools;
  auto summary_config = options_->generation;
  summary_config.max_tokens = std::min<std::size_t>(summary_config.max_tokens, 1024);
  if (older_tokens > 0)
    summary_config.max_tokens =
        std::min(summary_config.max_tokens, std::max<std::size_t>(32, older_tokens / 2));

  const GenerationRequest summary_measurement_request{
      summary_messages, no_tools, summary_config, options_->reasoning_effort
  };
  auto summary_metrics = model_->measure(summary_measurement_request);
  if (!summary_metrics) {
    emit(callback, AgentEvent{EventType::Error, summary_metrics.error().message, {}});
    return make_unexpected(summary_metrics.error());
  }
  const auto capacity = model_->context_size();
  if (capacity > 0) {
    if (summary_metrics.value().input_tokens >= capacity) {
      const auto error = context_limit_error(summary_metrics.value().input_tokens, 1, capacity);
      emit(callback, AgentEvent{EventType::Error, error.message, {}});
      return make_unexpected(error);
    }
    summary_config.max_tokens =
        std::min(summary_config.max_tokens, capacity - summary_metrics.value().input_tokens);
  }
  auto summary_response = model_->generate(
      GenerationRequest{summary_messages, no_tools, summary_config, options_->reasoning_effort},
      {},
      stop_token
  );
  while (!summary_response && summary_response.error().code == ErrorCode::ContextLimitExceeded
         && summary_config.max_tokens > 16) {
    summary_config.max_tokens = std::max<std::size_t>(16, summary_config.max_tokens / 2);
    summary_response = model_->generate(
        GenerationRequest{summary_messages, no_tools, summary_config, options_->reasoning_effort},
        {},
        stop_token
    );
  }
  if (!summary_response) {
    emit(callback, AgentEvent{EventType::Error, summary_response.error().message, {}});
    return make_unexpected(summary_response.error());
  }
  if (!summary_response.value().tool_calls.empty() || summary_response.value().content.empty()) {
    const Error error{
        ErrorCode::GenerationFailed, "compaction model did not return a text summary"
    };
    emit(callback, AgentEvent{EventType::Error, error.message, {}});
    return make_unexpected(error);
  }
  const std::string summary =
      "Earlier conversation was compacted.\n" + summary_response.value().content;
  std::vector<Message> recent(context_.begin() + recent_start, context_.end());
  std::vector<Message> candidate = context_;
  if (system_count) {
    candidate.front().content += "\n\n" + summary;
    candidate.erase(candidate.begin() + first_conversation_message, candidate.end());
  } else {
    candidate.clear();
    candidate.push_back(Message{Role::System, summary});
  }
  candidate.insert(candidate.end(), recent.begin(), recent.end());

  const GenerationRequest candidate_request{
      candidate, definitions, options_->generation, options_->reasoning_effort
  };
  auto candidate_metrics = model_->measure(candidate_request);
  if (!candidate_metrics) {
    emit(callback, AgentEvent{EventType::Error, candidate_metrics.error().message, {}});
    return make_unexpected(candidate_metrics.error());
  }
  const std::size_t candidate_tokens = candidate_metrics.value().input_tokens;
  if (candidate_tokens >= result.input_tokens_before) {
    emit(callback, AgentEvent{EventType::CompactionCompleted, "no reduction", {}});
    return result;
  }
  context_ = std::move(candidate);

  result.compacted = true;
  result.messages_after = context_.size();
  result.input_tokens_after = candidate_tokens;
  emit(
      callback,
      AgentEvent{EventType::CompactionCompleted, std::to_string(result.input_tokens_after), {}}
  );
  return result;
}

/**
 * @brief Clears the conversation history.
 *
 * This method removes all messages from the conversation's history. If a system
 * prompt is configured, it will be re-added to the history after clearing.
 */
void Conversation::clear() {
  history_.clear();
  context_.clear();
  const auto system_context = initial_system_context(*options_);
  if (!system_context.empty()) {
    history_.push_back(Message{Role::System, system_context});
    context_.push_back(history_.back());
  }
}

} // namespace juno::sdk
