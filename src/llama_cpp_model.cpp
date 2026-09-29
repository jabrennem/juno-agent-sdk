/**
 * @file llama_cpp_model.cpp
 * @brief Implements the LlamaCppModel class for inference using llama.cpp.
 *
 * This file contains the implementation of the LlamaCppModel class, which
 * provides an interface for performing inference using the llama.cpp library.
 * It handles model loading, prompt generation, tokenization, and response
 * parsing.
 *
 */

#include "juno_sdk/llama_cpp_model.hpp"

#include <fstream>
#include <mutex>
#include <string_view>
#include <utility>

#if JUNO_AGENT_HAS_LLAMA_CPP
#include <algorithm>
#include <chat.h>
#include <filesystem>
#include <llama.h>
#include <nlohmann/json.hpp>

// The llama.cpp library is used for inference with GGUF models. It provides
// functions for model loading, tokenization, and generation.
namespace juno::sdk {

// Implementation of the LlamaCppModel class, which manages inference sessions
// using llama.cpp.
namespace {

void append_prompt_log(const std::string &path, const std::string &prompt) {
  if (path.empty())
    return;
  std::ofstream output(path, std::ios::app);
  if (output)
    output << "\n===== Juno prompt =====\n" << prompt << "\n===== End Juno prompt =====\n";
}

// Returns the string representation of a Role enum value.
const char *role_name(const Role role) {
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
  return "user";
}

// Returns the string representation of a ReasoningEffort enum value.
const char *reasoning_effort_name(const ReasoningEffort effort) {
  switch (effort) {
  case ReasoningEffort::Low:
    return "low";
  case ReasoningEffort::Medium:
    return "medium";
  case ReasoningEffort::High:
    return "high";
  case ReasoningEffort::None:
    break;
  }
  return "none";
}

/** Returns the model-facing tool and memory instructions. */
std::string tool_protocol(const std::span<const ToolDefinition> tools) {
  if (tools.empty())
    return {};
  nlohmann::json definitions = nlohmann::json::array();
  bool has_memory_tool = false;
  for (const ToolDefinition &tool : tools) {
    nlohmann::json parameters = nlohmann::json::object();
    try {
      parameters = nlohmann::json::parse(tool.parameters_json);
    } catch (...) {
    }
    has_memory_tool = has_memory_tool || tool.name.find("memory") != std::string::npos
                      || tool.description.find("memory") != std::string::npos;
    definitions.push_back(
        {{"name", tool.name}, {"description", tool.description}, {"parameters", parameters}}
    );
  }

  std::string protocol =
      "\n\n<tools>\nAvailable tools (JSON): " + definitions.dump() + "\n</tools>\n";
  if (has_memory_tool) {
    protocol += R"(<memory_policy>
Use durable memory selectively. Automatically save stable facts the user volunteers about
themselves, including their preferences, identity, recurring context, and standing instructions,
when those facts are likely to improve a future conversation. Also save information when the
user explicitly asks you to remember it. Do not save one-off requests, temporary forecasts,
current conditions, or facts that are only useful for the current task. When in doubt, do not
save. Before updating an existing memory file, read it first and write back the complete merged
Markdown document so existing memories are preserved. Prefer MEMORY.md unless a more specific
Markdown file is clearly appropriate.
</memory_policy>
)";
  }
  protocol += R"(<response_protocol>
When a tool is needed, respond with only JSON in this shape:
{"tool_calls":[{"id":"unique-id","name":"tool-name","arguments":{}}]}
After a tool result, either call another tool if necessary or answer the user directly.
Do not emit empty or visible <think> blocks.
</response_protocol>
)";
  return protocol;
}

struct PreparedPrompt {
  std::string text;
  std::vector<llama_token> tokens;
};

Expected<PreparedPrompt> prepare_prompt(
    common_chat_templates *chat_templates,
    llama_model *model,
    const GenerationRequest &request
) {
  std::vector<common_chat_msg> chat;
  chat.reserve(request.messages.size() + 1);
  for (const Message &message : request.messages) {
    std::string text = message.content;
    if (message.role == Role::System)
      text += tool_protocol(request.tools);
    const Role rendered_role = message.role == Role::Tool ? Role::User : message.role;
    if (message.role == Role::Tool)
      text = "Tool result for " + message.tool_name + " (" + message.tool_call_id + "): " + text;
    common_chat_msg chat_message;
    chat_message.role = role_name(rendered_role);
    chat_message.content = std::move(text);
    chat.push_back(std::move(chat_message));
  }
  if (chat.empty() && !request.tools.empty()) {
    common_chat_msg chat_message;
    chat_message.role = "system";
    chat_message.content = tool_protocol(request.tools);
    chat.push_back(std::move(chat_message));
  }

  std::string prompt;
  try {
    common_chat_templates_inputs template_inputs;
    template_inputs.messages = std::move(chat);
    template_inputs.add_generation_prompt = true;
    template_inputs.use_jinja = true;
    template_inputs.enable_thinking = request.reasoning_effort != ReasoningEffort::None;
    if (template_inputs.enable_thinking) {
      template_inputs.chat_template_kwargs["reasoning_effort"] =
          nlohmann::json(reasoning_effort_name(request.reasoning_effort)).dump();
    }
    prompt = common_chat_templates_apply(chat_templates, template_inputs).prompt;
  } catch (const std::exception &exception) {
    return make_unexpected(
        Error{
            ErrorCode::GenerationFailed,
            std::string("llama.cpp could not apply the chat template: ") + exception.what()
        }
    );
  }
  if (prompt.empty()) {
    return make_unexpected(
        Error{ErrorCode::GenerationFailed, "llama.cpp produced an empty chat prompt"}
    );
  }

  const llama_vocab *vocab = llama_model_get_vocab(model);
  std::vector<llama_token> tokens(prompt.size() + 32);
  int token_count = llama_tokenize(
      vocab,
      prompt.c_str(),
      static_cast<int32_t>(prompt.size()),
      tokens.data(),
      static_cast<int32_t>(tokens.size()),
      true,
      true
  );
  if (token_count < 0) {
    tokens.resize(static_cast<std::size_t>(-token_count));
    token_count = llama_tokenize(
        vocab,
        prompt.c_str(),
        static_cast<int32_t>(prompt.size()),
        tokens.data(),
        static_cast<int32_t>(tokens.size()),
        true,
        true
    );
  }
  if (token_count <= 0) {
    return make_unexpected(
        Error{ErrorCode::GenerationFailed, "llama.cpp could not tokenize the prompt"}
    );
  }
  tokens.resize(static_cast<std::size_t>(token_count));
  return PreparedPrompt{std::move(prompt), std::move(tokens)};
}

/** Parses the model's output text to extract tool calls and content. */
Expected<GenerationResponse> parse_response(const std::string &text) {
  try {
    nlohmann::json parsed;
    std::string tool_call_text;

    // Models commonly put tool JSON after a private reasoning block, e.g.
    // `<think>...</think>{"tool_calls":[...]}`. Try the complete response
    // first, then each JSON object embedded in the response.
    try {
      parsed = nlohmann::json::parse(text);
      tool_call_text = text;
    } catch (const nlohmann::json::parse_error &) {
      for (std::size_t offset = text.find('{'); offset != std::string::npos;
           offset = text.find('{', offset + 1)) {
        try {
          auto candidate = nlohmann::json::parse(text.substr(offset));
          if (candidate.contains("tool_calls") && candidate.at("tool_calls").is_array()) {
            parsed = std::move(candidate);
            tool_call_text = text.substr(offset);
            break;
          }
        } catch (const nlohmann::json::parse_error &) {
          // Keep looking; an earlier brace may belong to the reasoning text.
        }
      }
      if (tool_call_text.empty())
        return GenerationResponse{text, {}};
    }

    if (!parsed.contains("tool_calls") || !parsed.at("tool_calls").is_array())
      return GenerationResponse{text, {}};
    // Keep only the assistant's tool-call JSON in the transcript. Reasoning is
    // already represented by the model's thinking prompt prefix and should
    // not be fed back inside a newly generated <think> block.
    GenerationResponse response;
    response.content = tool_call_text;
    for (const auto &item : parsed.at("tool_calls")) {
      if (!item.contains("name") || !item.at("name").is_string()) {
        return make_unexpected(
            Error{ErrorCode::InvalidModelOutput, "tool call did not include a string name"}
        );
      }
      ToolCall call;
      call.id = item.value("id", "call-" + std::to_string(response.tool_calls.size() + 1));
      call.name = item.at("name").get<std::string>();
      call.arguments_json = item.value("arguments", nlohmann::json::object()).dump();
      response.tool_calls.push_back(std::move(call));
    }
    return response;
  } catch (const nlohmann::json::parse_error &) {
    return GenerationResponse{text, {}};
  } catch (const std::exception &exception) {
    return make_unexpected(Error{ErrorCode::InvalidModelOutput, exception.what()});
  }
}

} // namespace

// Internal implementation details for LlamaCppModel.
struct LlamaCppModel::Impl {
  explicit Impl(LlamaCppOptions value) : config(std::move(value)) {}
  ~Impl() {
    chat_templates.reset();
    if (model)
      llama_model_free(model);
  }

  LlamaCppOptions config;
  llama_model *model{nullptr};
  common_chat_templates_ptr chat_templates;
  std::mutex mutex;
};

LlamaCppModel::LlamaCppModel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LlamaCppModel::~LlamaCppModel() = default;

std::size_t LlamaCppModel::context_size() const {
  return impl_->config.context_size;
}

Expected<PromptMetrics> LlamaCppModel::measure(const GenerationRequest &request) {
  std::scoped_lock lock(impl_->mutex);
  auto prepared = prepare_prompt(impl_->chat_templates.get(), impl_->model, request);
  if (!prepared)
    return make_unexpected(prepared.error());
  return PromptMetrics{prepared.value().tokens.size(), true};
}

std::shared_ptr<LlamaCppModel> LlamaCppModel::create(LlamaCppOptions config) {
  if (config.model_path.empty())
    throw ConfigurationError(
        Error{ErrorCode::InvalidConfiguration, "a GGUF model path is required"}
    );
  if (!std::filesystem::exists(config.model_path))
    throw ModelError(Error{ErrorCode::ModelLoadFailed, "GGUF model file does not exist"});
  llama_backend_init();
  auto impl = std::make_unique<Impl>(std::move(config));
  auto params = llama_model_default_params();
  impl->model = llama_model_load_from_file(impl->config.model_path.c_str(), params);
  if (!impl->model)
    throw ModelError(Error{ErrorCode::ModelLoadFailed, "llama.cpp could not load the GGUF model"});
  try {
    impl->chat_templates =
        common_chat_templates_init(impl->model, impl->config.chat_template_override);
  } catch (const std::exception &exception) {
    throw ConfigurationError(
        Error{
            ErrorCode::InvalidConfiguration,
            std::string("llama.cpp could not initialize the chat template: ") + exception.what()
        }
    );
  }
  return std::shared_ptr<LlamaCppModel>(new LlamaCppModel(std::move(impl)));
}

/**
 * This is a heavy function that performs the entire inference process,
 * from prompt generation to token sampling and response parsing.
 *
 * Generates a response from the model based on the provided request.
 *
 * @param request The generation request containing messages and configuration.
 * @param callback A callback function to receive streaming events during
 * generation.
 * @param stop_token A stop token to allow cancellation of the generation
 * process.
 * @return An Expected object containing either a GenerationResponse or an
 * Error.
 */
Expected<GenerationResponse> LlamaCppModel::generate(
    const GenerationRequest &request,
    const EventCallback &callback,
    std::stop_token stop_token
) {

  // Lock the mutex to ensure thread safety during generation.
  std::scoped_lock lock(impl_->mutex);
  if (stop_token.stop_requested())
    return make_unexpected(Error{ErrorCode::Cancelled, "generation was cancelled"});

  // Render and tokenize through the same path used by preflight measurement.
  auto prepared = prepare_prompt(impl_->chat_templates.get(), impl_->model, request);
  if (!prepared)
    return make_unexpected(prepared.error());
  append_prompt_log(impl_->config.prompt_log_path, prepared.value().text);
  if (callback)
    callback(AgentEvent{EventType::Prompt, prepared.value().text, {}});

  auto &tokens = prepared.value().tokens;
  const llama_vocab *vocab = llama_model_get_vocab(impl_->model);
  if (tokens.size() + request.config.max_tokens > impl_->config.context_size) {
    return make_unexpected(
        Error{
            ErrorCode::ContextLimitExceeded,
            "prompt and generation budget exceed the configured context"
        }
    );
  }

  // 4. Create an inference context and load the prompt into it.
  auto context_params = llama_context_default_params();
  context_params.n_ctx = static_cast<uint32_t>(impl_->config.context_size);
  // Keep batch_size as the caller's memory/throughput knob. A prompt larger
  // than one batch is decoded below in multiple chunks.
  context_params.n_batch = static_cast<uint32_t>(
      std::min(std::max<std::size_t>(1, impl_->config.batch_size), impl_->config.context_size)
  );
  if (impl_->config.threads != 0) {
    context_params.n_threads = static_cast<int32_t>(impl_->config.threads);
    context_params.n_threads_batch = static_cast<int32_t>(impl_->config.threads);
  }
  llama_context *context = llama_init_from_model(impl_->model, context_params);
  if (!context)
    return make_unexpected(
        Error{ErrorCode::GenerationFailed, "llama.cpp could not create an inference context"}
    );
  struct ContextGuard {
    llama_context *value;
    ~ContextGuard() {
      llama_free(value);
    }
  } context_guard{context};

  for (std::size_t offset = 0; offset < tokens.size();) {
    const auto chunk_size = std::min<std::size_t>(context_params.n_batch, tokens.size() - offset);
    const int decode_result = llama_decode(
        context, llama_batch_get_one(tokens.data() + offset, static_cast<int32_t>(chunk_size))
    );
    if (decode_result != 0)
      return make_unexpected(
          Error{ErrorCode::GenerationFailed, "llama.cpp failed to decode the prompt"}
      );
    offset += chunk_size;
  }

  // 5. Configure the sampler that chooses each next token.
  auto sampler_params = llama_sampler_chain_default_params();
  llama_sampler *sampler = llama_sampler_chain_init(sampler_params);
  llama_sampler_chain_add(sampler, llama_sampler_init_top_p(request.config.top_p, 1));
  llama_sampler_chain_add(sampler, llama_sampler_init_temp(request.config.temperature));
  llama_sampler_chain_add(sampler, llama_sampler_init_dist(request.config.seed));
  struct SamplerGuard {
    llama_sampler *value;
    ~SamplerGuard() {
      llama_sampler_free(value);
    }
  } sampler_guard{sampler};

  // 6. Generate, stream, and decode one token at a time.
  std::string generated;
  std::string stream_buffer;
  constexpr std::string_view think_open = "<think>";
  constexpr std::string_view think_close = "</think>";
  bool in_reasoning = request.reasoning_effort != ReasoningEffort::None;
  bool checked_think_open = !in_reasoning;
  std::string pending_tool_text;
  const auto emit_text = [&](const std::string_view text) {
    pending_tool_text.append(text);
    const std::size_t first_content = pending_tool_text.find_first_not_of(" \t\r\n");
    if (first_content == std::string::npos)
      return;

    if (pending_tool_text[first_content] == '{')
      return;

    std::string ready_text = std::move(pending_tool_text);
    pending_tool_text.clear();
    if (callback && !ready_text.empty())
      callback(AgentEvent{EventType::TextDelta, std::move(ready_text), {}});
  };
  const auto emit_stream = [&](const std::string_view delta) {
    stream_buffer.append(delta);

    if (in_reasoning && !checked_think_open) {
      if (stream_buffer.size() < think_open.size()
          && think_open.substr(0, stream_buffer.size()) == stream_buffer)
        return;
      if (stream_buffer.starts_with(think_open))
        stream_buffer.erase(0, think_open.size());
      checked_think_open = true;
    }

    while (in_reasoning) {
      const std::size_t close = stream_buffer.find(think_close);
      if (close != std::string::npos) {
        if (close > 0 && callback)
          callback(AgentEvent{EventType::ReasoningDelta, stream_buffer.substr(0, close), {}});
        stream_buffer.erase(0, close + think_close.size());
        in_reasoning = false;
        break;
      }

      std::size_t keep = 0;
      const std::size_t max_suffix = std::min(stream_buffer.size(), think_close.size() - 1);
      for (std::size_t length = max_suffix; length > 0; --length) {
        if (think_close.substr(0, length)
            == std::string_view(stream_buffer).substr(stream_buffer.size() - length)) {
          keep = length;
          break;
        }
      }
      if (stream_buffer.size() > keep && callback) {
        callback(
            AgentEvent{
                EventType::ReasoningDelta, stream_buffer.substr(0, stream_buffer.size() - keep), {}
            }
        );
        stream_buffer.erase(0, stream_buffer.size() - keep);
      }
      return;
    }

    if (!stream_buffer.empty())
      emit_text(stream_buffer);
    stream_buffer.clear();
  };
  int32_t position = static_cast<int32_t>(tokens.size());
  for (std::size_t i = 0; i < request.config.max_tokens; ++i) {
    if (stop_token.stop_requested())
      return make_unexpected(Error{ErrorCode::Cancelled, "generation was cancelled"});
    const llama_token token = llama_sampler_sample(sampler, context, -1);
    if (llama_vocab_is_eog(vocab, token))
      break;
    llama_sampler_accept(sampler, token);
    std::vector<char> piece(32);
    int32_t piece_size = llama_token_to_piece(
        vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, true
    );
    if (piece_size < 0) {
      piece.resize(static_cast<std::size_t>(-piece_size));
      piece_size = llama_token_to_piece(
          vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, true
      );
    }
    if (piece_size > 0) {
      const std::string delta(piece.data(), static_cast<std::size_t>(piece_size));
      generated += delta;
      emit_stream(delta);
    }
    llama_token decoded_token = token;
    if (llama_decode(context, llama_batch_get_one(&decoded_token, 1)) != 0) {
      return make_unexpected(
          Error{ErrorCode::GenerationFailed, "llama.cpp failed during token generation"}
      );
    }
    ++position;
  }

  if (!stream_buffer.empty()) {
    if (in_reasoning) {
      if (callback)
        callback(AgentEvent{EventType::ReasoningDelta, std::move(stream_buffer), {}});
    } else {
      emit_text(stream_buffer);
    }
  }

  // 7. Interpret the completed text as plain content or tool calls.
  auto response = parse_response(generated);
  if (!response)
    return response;
  if (!pending_tool_text.empty() && response.value().tool_calls.empty() && callback)
    callback(AgentEvent{EventType::TextDelta, std::move(pending_tool_text), {}});
  return response;
}

} // namespace juno::sdk

#else

namespace juno::sdk {
struct LlamaCppModel::Impl {};
LlamaCppModel::LlamaCppModel(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LlamaCppModel::~LlamaCppModel() = default;
std::size_t LlamaCppModel::context_size() const {
  return 0;
}
Expected<PromptMetrics> LlamaCppModel::measure(const GenerationRequest &) {
  return make_unexpected(
      Error{ErrorCode::ModelUnavailable, "llama.cpp support was not compiled into this build"}
  );
}
std::shared_ptr<LlamaCppModel> LlamaCppModel::create(LlamaCppOptions) {
  throw ModelError(
      Error{
          ErrorCode::ModelUnavailable,
          "rebuild with JUNO_AGENT_ENABLE_LLAMA_CPP=ON to use llama.cpp"
      }
  );
}
Expected<GenerationResponse>
LlamaCppModel::generate(const GenerationRequest &, const EventCallback &, std::stop_token) {
  return make_unexpected(
      Error{ErrorCode::ModelUnavailable, "llama.cpp support was not compiled into this build"}
  );
}
} // namespace juno::sdk

#endif
