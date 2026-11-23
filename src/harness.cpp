#include "juno_sdk/harness.hpp"

#include <iomanip>
#include <sstream>
#include <utility>

namespace juno::sdk {
namespace {

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::string format_token_count(std::size_t value) {
  std::string result = std::to_string(value);
  for (std::size_t offset = result.size(); offset > 3; offset -= 3)
    result.insert(offset - 3, ",");
  return result;
}

std::string format_threshold_usage(const ContextUsage &usage) {
  if (usage.compaction_threshold == 0)
    return "automatic compaction disabled";
  std::ostringstream output;
  output << std::fixed << std::setprecision(1)
         << 100.0 * static_cast<double>(usage.input_tokens) /
                static_cast<double>(usage.compaction_threshold)
         << "% of compaction threshold";
  return output.str();
}

} // namespace

Harness Harness::create(HarnessOptions options) { return Harness(std::move(options)); }

Harness::Harness(HarnessOptions options)
    : options_(std::move(options)), conversation_(options_.agent.start_conversation()) {}

Expected<RunResult> Harness::invoke(std::string_view prompt, std::stop_token stop_token) {
  auto *output = options_.output;
  bool line_open = false;
  bool streamed_text = false;
  const auto finish_line = [&] {
    if (line_open) {
      *output << '\n';
      line_open = false;
    }
  };

  EventCallback render;
  if (output) {
    render = [&](const AgentEvent &event) {
      switch (event.type) {
      case EventType::TextDelta:
      case EventType::ReasoningDelta:
        if (event.text.empty() ||
            (event.type == EventType::ReasoningDelta && !options_.show_reasoning))
          break;
        *output << event.text << std::flush;
        line_open = event.text.back() != '\n';
        streamed_text |= event.type == EventType::TextDelta;
        break;
      case EventType::ToolStarted:
        streamed_text = false;
        if (options_.show_tools) {
          finish_line();
          *output << "tool: " << event.tool_call.name << '(' << event.tool_call.arguments_json
                  << ")\n"
                  << std::flush;
        }
        break;
      case EventType::ToolCompleted:
        if (options_.show_tools)
          *output << "tool completed: " << event.tool_call.name << '\n' << std::flush;
        break;
      case EventType::CompactionStarted:
        if (options_.compaction) {
          finish_line();
          *output << "Compacting conversation...\n" << std::flush;
        }
        break;
      case EventType::CompactionCompleted:
        if (options_.compaction && event.text != "no reduction")
          *output << "Conversation compacted.\n" << std::flush;
        break;
      default:
        break;
      }
    };
  }

  auto candidate = conversation_;
  auto result = candidate.run(prompt, render, stop_token);
  if (result && stop_token.stop_requested())
    result = Error{ErrorCode::Cancelled, "agent run was cancelled"};
  if (result && output) {
    const auto &text = result.value().final_text;
    const auto &display = trim(text).empty() ? options_.empty_response : text;
    if (!display.empty() && (!streamed_text || trim(text).empty())) {
      finish_line();
      *output << display;
      line_open = display.back() != '\n';
    }
  }
  finish_line();
  if (output)
    *output << std::flush;
  if (!result)
    return result;

  conversation_ = std::move(candidate);
  return result;
}

void Harness::new_session() { conversation_.clear(); }

Expected<void> Harness::run_interactive(std::istream &input) {
  auto *output = options_.output;
  print_help();
  for (std::string line;;) {
    if (output)
      *output << options_.prompt << std::flush;
    if (!std::getline(input, line))
      break;
    const auto command = trim(line);
    if (command.empty()) {
      continue;
    } else if (command == "/quit" || command == "/exit") {
      return {};
    } else if (command == "/help") {
      print_help();
    } else if (command == "/tools") {
      print_tools();
    } else if (command == "/history") {
      if (output)
        *output << conversation_.history().size() << " messages\n";
    } else if (command == "/new" || command == "/clear") {
      new_session();
      if (output)
        *output << "Started a new conversation.\n";
    } else if (options_.compaction && command == "/compact") {
      compact();
    } else if (options_.compaction && command == "/context") {
      print_context();
    } else {
      if (output)
        *output << "assistant:\n";
      auto result = invoke(line);
      if (!result)
        report_error("Run failed", result.error());
    }
  }
  if (input.bad() || !input.eof())
    return Error{ErrorCode::InvalidConfiguration, "could not read terminal input"};
  return {};
}

void Harness::print_help() const {
  auto *output = options_.output;
  if (!output)
    return;
  if (!options_.help.empty())
    *output << options_.help << '\n';
  *output << "Commands: /help, /tools, /history, /new (/clear), /quit (/exit)";
  if (options_.compaction)
    *output << ", /compact, /context";
  *output << '\n';
}

void Harness::print_tools() const {
  auto *output = options_.output;
  if (!output)
    return;
  const auto tools = options_.agent.tool_definitions();
  if (tools.empty()) {
    *output << "No tools configured.\n";
    return;
  }
  *output << "Available tools:\n";
  for (const auto &tool : tools)
    *output << "- " << tool.name << ": " << tool.description << '\n';
}

void Harness::print_context() const {
  auto *output = options_.output;
  if (!output)
    return;
  const auto usage = conversation_.context_usage();
  if (!usage) {
    report_error("Could not measure context", usage.error());
    return;
  }
  const auto &value = usage.value();
  *output << "Rendered prompt: " << format_token_count(value.input_tokens) << " tokens ("
          << format_threshold_usage(value) << ")\n"
          << "Compaction threshold: " << format_token_count(value.compaction_threshold)
          << " tokens\n"
          << "Output reservation: " << format_token_count(value.output_reservation) << " tokens\n"
          << "Context capacity: " << format_token_count(value.context_capacity) << " tokens\n";
}

void Harness::compact() {
  auto *output = options_.output;
  auto candidate = conversation_;
  const auto result = candidate.compact(*options_.compaction, [&](const AgentEvent &event) {
    if (output && event.type == EventType::CompactionStarted)
      *output << "Compacting conversation...\n" << std::flush;
  });
  if (!result) {
    report_error("Compaction failed", result.error());
    return;
  }
  const auto &value = result.value();
  if (!value.compacted) {
    if (output)
      *output << "Compaction made no reduction.\n";
    return;
  }
  conversation_ = std::move(candidate);
  if (output) {
    *output << "Compacted model context from " << value.messages_before << " to "
            << value.messages_after << " messages (" << value.input_tokens_before << " to "
            << value.input_tokens_after << " rendered prompt tokens). Full history is preserved.\n";
  }
}

void Harness::report_error(std::string_view operation, const Error &error) const {
  if (options_.output)
    *options_.output << operation << ": " << error.message << '\n';
}

} // namespace juno::sdk
