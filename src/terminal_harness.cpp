#include "juno_sdk/terminal_harness.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <utility>

#ifdef JUNO_AGENT_HAS_EDITLINE
#include <array>
#include <cstdio>
#include <histedit.h>
#include <unistd.h>
#endif

namespace juno::sdk {
namespace {

// Foreground colors from daw-agent's Production DAW palette.
constexpr std::string_view kAccent = "\033[38;2;90;167;255m";
constexpr std::string_view kText = "\033[38;2;229;233;239m";
constexpr std::string_view kMuted = "\033[38;2;154;164;175m";
constexpr std::string_view kSuccess = "\033[38;2;95;210;143m";
constexpr std::string_view kError = "\033[38;2;255;107;107m";
constexpr std::string_view kReset = "\033[0m";

std::ostream &write_colored(const TerminalHarnessOptions &options, std::string_view color,
                            const auto &...parts) {
  auto &output = *options.output;
  if (options.color)
    output << color;
  (output << ... << parts);
  if (options.color)
    output << kReset;
  return output;
}

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

class TerminalInput {
public:
  TerminalInput(std::istream &input, const TerminalHarnessOptions &options)
      : input_(input), options_(options) {
#ifdef JUNO_AGENT_HAS_EDITLINE
    if (&input != &std::cin || options.output != &std::cout || !isatty(fileno(stdin)) ||
        !isatty(fileno(stdout)))
      return;

    prompt_ = options.color && !options.prompt.empty()
                  ? "\001" + std::string(kAccent) + "\001" + options.prompt
                  : options.prompt;
    auto *editor = el_init("juno", stdin, stdout, stderr);
    if (!editor)
      return;
    if (el_set(editor, EL_CLIENTDATA, this) == -1 ||
        el_set(editor, EL_PROMPT_ESC, &TerminalInput::prompt, '\001') == -1 ||
        el_set(editor, EL_ADDFN, "juno-complete", "Complete a slash command",
               &TerminalInput::complete) == -1 ||
        el_set(editor, EL_BIND, "^I", "juno-complete", static_cast<const char *>(nullptr)) == -1) {
      el_end(editor);
      return;
    }
    editor_.reset(editor);
#endif
  }

  Expected<bool> read(std::string &line) {
#ifdef JUNO_AGENT_HAS_EDITLINE
    if (editor_) {
      std::cout.flush();
      int count = 0;
      const char *read = el_gets(editor_.get(), &count);
      if (options_.color && !prompt_.empty())
        std::cout << kReset << std::flush;
      if (read && count > 0) {
        line.assign(read, static_cast<std::size_t>(count));
        if (line.back() == '\n')
          line.pop_back();
        return true;
      }
      if (count >= 0)
        return false;
      return Error{ErrorCode::InvalidConfiguration, "could not read terminal input"};
    }
#endif
    if (auto *output = options_.output) {
      if (!options_.prompt.empty())
        write_colored(options_, kAccent, options_.prompt);
      output->flush();
    }
    if (std::getline(input_, line))
      return true;
    if (input_.bad() || !input_.eof())
      return Error{ErrorCode::InvalidConfiguration, "could not read terminal input"};
    return false;
  }

private:
#ifdef JUNO_AGENT_HAS_EDITLINE
  static TerminalInput &state(EditLine *editor) {
    void *data = nullptr;
    el_get(editor, EL_CLIENTDATA, &data);
    return *static_cast<TerminalInput *>(data);
  }

  static char *prompt(EditLine *editor) { return state(editor).prompt_.data(); }

  static unsigned char complete(EditLine *editor, int) {
    const auto *line = el_line(editor);
    if (line->cursor != line->lastchar)
      return CC_NORM;
    const auto prefix = trim(std::string_view(line->buffer, line->lastchar - line->buffer));
    if (!prefix.starts_with('/') || prefix.data() + prefix.size() != line->lastchar)
      return CC_NORM;

    const auto &options = state(editor).options_;
    // A trailing space helps users enter arguments after completing these commands.
    constexpr std::array common{"/help", "/tools", "/history", "/new", "/quit", "/clear", "/exit"};
    constexpr std::array compaction{"/compact", "/context"};
    constexpr std::array sessions{"/sessions ", "/resume ", "/delete "};
    std::array<const char *, common.size() + compaction.size() + sessions.size()> matches;
    std::size_t count = 0;
    const auto collect = [&](const auto &commands) {
      for (const auto *command : commands) {
        if (std::string_view(command).starts_with(prefix))
          matches[count++] = command;
      }
    };
    collect(common);
    if (options.compaction)
      collect(compaction);
    if (options.session_store)
      collect(sessions);
    if (count == 0)
      return CC_NORM;
    if (count == 1) {
      const auto *suffix = matches[0] + prefix.size();
      return *suffix && el_insertstr(editor, suffix) == 0 ? CC_REFRESH : CC_NORM;
    }

    std::fputc('\n', stdout);
    for (std::size_t i = 0; i < count; ++i) {
      const auto command = trim(matches[i]);
      std::fprintf(stdout, "%.*s%s", static_cast<int>(command.size()), command.data(),
                   i + 1 == count ? "\n" : "  ");
    }
    std::fflush(stdout);
    return CC_REDISPLAY;
  }

  std::string prompt_;
  std::unique_ptr<EditLine, decltype(&el_end)> editor_{nullptr, &el_end};
#endif
  std::istream &input_;
  const TerminalHarnessOptions &options_;
};

} // namespace

TerminalHarness TerminalHarness::create(TerminalHarnessOptions options) {
  return TerminalHarness(std::move(options));
}

TerminalHarness::TerminalHarness(TerminalHarnessOptions options)
    : options_(std::move(options)), conversation_(options_.agent.start_conversation()) {}

Expected<RunResult> TerminalHarness::invoke(std::string_view prompt, std::stop_token stop_token) {
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
        write_colored(options_, event.type == EventType::TextDelta ? kText : kMuted, event.text)
            << std::flush;
        line_open = event.text.back() != '\n';
        streamed_text |= event.type == EventType::TextDelta;
        break;
      case EventType::ToolStarted:
        streamed_text = false;
        if (options_.show_tools) {
          finish_line();
          write_colored(options_, kAccent, "tool: ", event.tool_call.name, '(',
                        event.tool_call.arguments_json, ')')
              << '\n'
              << std::flush;
        }
        break;
      case EventType::ToolCompleted:
        if (options_.show_tools)
          write_colored(options_, kSuccess, "tool completed: ", event.tool_call.name) << '\n'
                                                                                      << std::flush;
        break;
      case EventType::CompactionStarted:
        if (options_.compaction) {
          finish_line();
          write_colored(options_, kAccent, "Compacting conversation...") << '\n' << std::flush;
        }
        break;
      case EventType::CompactionCompleted:
        if (options_.compaction && event.text != "no reduction")
          write_colored(options_, kSuccess, "Conversation compacted.") << '\n' << std::flush;
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
    const bool blank = trim(text).empty();
    const auto &display = blank ? options_.empty_response : text;
    if (!display.empty() && (!streamed_text || blank)) {
      finish_line();
      write_colored(options_, kText, display);
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

Expected<void> TerminalHarness::checkpoint(const Conversation &conversation) {
  if (!options_.session_store)
    return {};
  auto saved = options_.session_store->save(conversation.snapshot(), session_id_);
  if (!saved)
    return saved.error();
  session_id_ = std::move(saved.value().id);
  return {};
}

void TerminalHarness::new_session() {
  conversation_.clear();
  session_id_.reset();
}

Expected<void> TerminalHarness::resume_session(std::string_view id) {
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

const std::optional<std::string> &TerminalHarness::active_session_id() const { return session_id_; }

// Run an interactive loop reading prompts and commands from the user.
Expected<void> TerminalHarness::run_interactive(std::istream &input) {
  // Print the optional banner once at the start of the interactive loop.
  auto *output = options_.output;
  if (output && !options_.banner.empty())
    write_colored(options_, kAccent, options_.banner) << '\n';
  print_help();
  // Read lines from the terminal until EOF or a quit command is received.
  TerminalInput terminal(input, options_);
  // The user can enter a prompt to invoke the agent, or a slash command to perform an action.
  for (std::string line;;) {
    auto read = terminal.read(line);
    if (!read)
      return read.error();
    if (!read.value())
      return {};
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
        write_colored(options_, kAccent, "assistant:") << '\n';
      auto result = invoke(line);
      if (!result)
        report_error("Run failed", result.error());
    }
  }
}

void TerminalHarness::print_help() const {
  auto *output = options_.output;
  if (!output)
    return;
  if (!options_.help.empty())
    write_colored(options_, kMuted, options_.help) << '\n';
  write_colored(options_, kAccent, "Commands:")
      << " /help, /tools, /history, /new (/clear), /quit (/exit)";
  if (options_.compaction)
    *output << ", /compact, /context";
  *output << '\n';
  if (options_.session_store)
    write_colored(options_, kAccent, "Saved sessions:")
        << " /sessions [page], /resume ID, /delete ID\n";
}

void TerminalHarness::print_tools() const {
  auto *output = options_.output;
  if (!output)
    return;
  const auto tools = options_.agent.tool_definitions();
  if (tools.empty()) {
    *output << "No tools configured.\n";
    return;
  }
  write_colored(options_, kAccent, "Available tools:") << '\n';
  for (const auto &tool : tools)
    *output << "- " << tool.name << ": " << tool.description << '\n';
}

void TerminalHarness::print_context() const {
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

void TerminalHarness::compact() {
  auto *output = options_.output;
  auto candidate = conversation_;
  const auto result = candidate.compact(*options_.compaction, [&](const AgentEvent &event) {
    if (output && event.type == EventType::CompactionStarted)
      write_colored(options_, kAccent, "Compacting conversation...") << '\n' << std::flush;
  });
  if (!result) {
    report_error("Compaction failed", result.error());
    return;
  }
  const auto &value = result.value();
  if (!value.compacted) {
    if (output)
      write_colored(options_, kMuted, "Compaction made no reduction.") << '\n';
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
    write_colored(options_, kSuccess, "Compacted model context from ", value.messages_before,
                  " to ", value.messages_after, " messages (", value.input_tokens_before, " to ",
                  value.input_tokens_after, " rendered prompt tokens). Full history is preserved.")
        << '\n';
  }
}

void TerminalHarness::print_sessions(std::string_view argument) const {
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

void TerminalHarness::print_recent_exchanges() const {
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

void TerminalHarness::delete_session(std::string_view id) {
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

void TerminalHarness::report_error(std::string_view operation, const Error &error) const {
  if (options_.output)
    write_colored(options_, kError, operation, ": ", error.message) << '\n';
}

} // namespace juno::sdk
