#include "juno_sdk/harness.hpp"

#include <algorithm>
#include <charconv>
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

  auto saved = checkpoint(candidate);
  if (!saved)
    return saved.error();
  conversation_ = std::move(candidate);
  return result;
}

Expected<void> Harness::checkpoint(const Conversation &conversation) {
  if (!options_.session_store)
    return {};
  auto saved = options_.session_store->save(conversation.snapshot(), session_id_);
  if (!saved)
    return saved.error();
  session_id_ = std::move(saved.value().id);
  return {};
}

void Harness::new_session() {
  conversation_.clear();
  session_id_.reset();
}

Expected<void> Harness::resume_session(std::string_view id) {
  if (!options_.session_store)
    return Error{ErrorCode::InvalidConfiguration, "no session store configured"};
  auto saved = options_.session_store->load(id);
  if (!saved)
    return saved.error();
  auto restored = options_.agent.restore_conversation(std::move(saved.value().snapshot));
  if (!restored)
    return restored.error();
  conversation_ = std::move(restored.value());
  session_id_ = std::move(saved.value().info.id);
  return {};
}

const std::optional<std::string> &Harness::active_session_id() const { return session_id_; }

Expected<void> Harness::run_interactive(std::istream &input) {
  auto *output = options_.output;
  print_help();
  for (std::string line;;) {
    if (output)
      *output << options_.prompt << std::flush;
    if (!std::getline(input, line))
      break;
    const auto command = trim(line);
    const auto separator = command.find_first_of(" \t");
    const auto name = command.substr(0, separator);
    const auto argument =
        separator == std::string_view::npos ? "" : trim(command.substr(separator));
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
    } else if (options_.session_store && name == "/sessions") {
      print_sessions(argument);
    } else if (options_.session_store && name == "/resume") {
      auto result = resume_session(argument);
      if (result)
        print_recent_exchanges();
      else
        report_error("Could not resume session", result.error());
    } else if (options_.session_store && name == "/delete") {
      delete_session(argument);
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
  if (options_.session_store)
    *output << "Saved sessions: /sessions [page], /resume ID, /delete ID\n";
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
  if (session_id_) {
    auto saved = checkpoint(candidate);
    if (!saved) {
      report_error("Could not save compaction", saved.error());
      return;
    }
  }
  conversation_ = std::move(candidate);
  if (output) {
    *output << "Compacted model context from " << value.messages_before << " to "
            << value.messages_after << " messages (" << value.input_tokens_before << " to "
            << value.input_tokens_after << " rendered prompt tokens). Full history is preserved.\n";
  }
}

void Harness::print_sessions(std::string_view argument) const {
  auto *output = options_.output;
  if (!output)
    return;
  std::size_t page = 1;
  if (!argument.empty()) {
    const auto end = argument.data() + argument.size();
    const auto parsed = std::from_chars(argument.data(), end, page);
    if (parsed.ec != std::errc{} || parsed.ptr != end || page == 0) {
      *output << "Usage: /sessions [positive page number]\n";
      return;
    }
  }
  const auto result = options_.session_store->list();
  if (!result) {
    report_error("Could not list sessions", result.error());
    return;
  }
  const auto &sessions = result.value();
  if (sessions.empty()) {
    *output << "No saved sessions.\n";
    return;
  }
  constexpr std::size_t page_size = 20;
  const auto page_count = 1 + (sessions.size() - 1) / page_size;
  if (page > page_count) {
    *output << "Session page out of range (1-" << page_count << ").\n";
    return;
  }
  *output << "Saved sessions (page " << page << " of " << page_count << ")\n";
  const auto first = (page - 1) * page_size;
  const auto end = first + std::min(page_size, sessions.size() - first);
  for (auto index = first; index < end; ++index) {
    const auto &session = sessions[index];
    *output << session.id << "  " << session.title;
    if (session.id == session_id_)
      *output << " (active)";
    *output << '\n';
  }
  *output << "Use /resume ID to continue a session.\n";
  if (page < page_count)
    *output << "Use /sessions " << page + 1 << " to browse older sessions.\n";
}

void Harness::print_recent_exchanges() const {
  auto *output = options_.output;
  if (!output)
    return;
  *output << "Resumed session " << *session_id_ << '\n';
  const auto &history = conversation_.history();
  auto first = history.end();
  std::size_t turns = 0;
  while (first != history.begin()) {
    --first;
    if (first->role == Role::User && ++turns == 2)
      break;
  }
  for (; first != history.end(); ++first) {
    if (first->role == Role::User)
      *output << "user: " << first->content << '\n';
    else if (first->role == Role::Assistant && first->tool_calls.empty())
      *output << "assistant: " << first->content << '\n';
  }
}

void Harness::delete_session(std::string_view id) {
  auto erased = options_.session_store->erase(id);
  if (!erased) {
    report_error("Could not delete session", erased.error());
    return;
  }
  if (id == session_id_)
    new_session();
  if (options_.output)
    *options_.output << "Deleted session " << id << '\n';
}

void Harness::report_error(std::string_view operation, const Error &error) const {
  if (options_.output)
    *options_.output << operation << ": " << error.message << '\n';
}

} // namespace juno::sdk
