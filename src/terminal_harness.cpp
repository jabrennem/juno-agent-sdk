#include "juno_sdk/terminal_harness.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <utility>

#include "terminal_markdown.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#ifdef JUNO_AGENT_HAS_EDITLINE
#include <cstdio>
#include <histedit.h>
#endif

namespace juno::sdk {
namespace {

constexpr std::string_view kAccent = "\033[34m";
constexpr std::string_view kTitle = "\033[1;34m";
constexpr std::string_view kMuted = "\033[2m";
constexpr std::string_view kSuccess = "\033[32m";
constexpr std::string_view kError = "\033[31m";
constexpr std::string_view kReset = "\033[0m";

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

Milliseconds elapsed_since(Clock::time_point start) {
  return std::chrono::duration_cast<Milliseconds>(Clock::now() - start);
}

std::string format_elapsed(Milliseconds elapsed) {
  if (elapsed == Milliseconds::zero())
    return "<1 ms";
  if (elapsed < std::chrono::seconds(1))
    return std::to_string(elapsed.count()) + " ms";
  std::ostringstream output;
  output << std::fixed << std::setprecision(1) << std::chrono::duration<double>(elapsed).count()
         << " s";
  return output.str();
}

std::string local_time_label() {
#if defined(__unix__) || defined(__APPLE__)
  const auto now = std::time(nullptr);
  std::tm local{};
  if (localtime_r(&now, &local)) {
    std::ostringstream output;
    output << std::put_time(&local, "%H:%M:%S");
    return output.str();
  }
#endif
  return {};
}

bool terminal_output(const TerminalHarnessOptions &options) {
#if defined(__unix__) || defined(__APPLE__)
  const auto *term = std::getenv("TERM");
  return options.output == &std::cout && isatty(STDOUT_FILENO) &&
         (!term || std::string_view(term) != "dumb");
#else
  return false;
#endif
}

bool terminal_input(std::istream &input) {
#if defined(__unix__) || defined(__APPLE__)
  return &input == &std::cin && isatty(STDIN_FILENO);
#else
  return false;
#endif
}

bool color_enabled(const TerminalHarnessOptions &options) {
  const auto *no_color = std::getenv("NO_COLOR");
  return options.color && (!no_color || *no_color == '\0') && terminal_output(options);
}

std::size_t terminal_width() {
#if defined(__unix__) || defined(__APPLE__)
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
    return size.ws_col;
#endif
  return 80;
}

std::ostream &write_colored(const TerminalHarnessOptions &options, std::string_view color,
                            const auto &...parts) {
  auto &output = *options.output;
  const bool enabled = color_enabled(options);
  if (enabled)
    output << color;
  (output << ... << parts);
  if (enabled)
    output << kReset;
  return output;
}

class StatusLine {
public:
  StatusLine(std::ostream *output, std::size_t frame) : output_(output), frame_(frame) {}
  ~StatusLine() {
    if (worker_.joinable()) {
      {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
      }
      changed_.notify_one();
      worker_.join();
    }
    clear();
  }

  void show(std::string_view label) {
    if (!output_)
      return;
    const std::lock_guard lock(mutex_);
    thinking_ = false;
    redraw(label);
    changed_.notify_one();
  }

  void think() {
    if (!output_)
      return;
    const std::lock_guard lock(mutex_);
    if (thinking_)
      return;
    if (!worker_.joinable())
      worker_ = std::thread([this] { animate(); });
    thinking_ = true;
    ++thinking_sequence_;
    dots_frame_ = 0;
    redraw_thinking();
    changed_.notify_one();
  }

  void clear() {
    if (!output_)
      return;
    const std::lock_guard lock(mutex_);
    thinking_ = false;
    erase();
    changed_.notify_one();
  }

  std::size_t next_frame() {
    const std::lock_guard lock(mutex_);
    return (frame_ + 1) % kThinkingFrames.size();
  }

private:
  // Hold the mutex around every redraw. The main thread clears the status before
  // writing any transcript output, so the animation cannot overwrite it.
  void erase() {
    if (!visible_)
      return;
    *output_ << "\r\033[2K" << std::flush;
    visible_ = false;
  }

  void redraw(std::string_view label, std::string_view suffix = {}) {
    erase();
    const auto ascii_columns =
        std::ranges::count_if(label, [](unsigned char byte) { return byte < 0x80; });
    const auto max_columns = 2 * detail::terminal_text_width(label) - ascii_columns;
    if (max_columns + suffix.size() >= terminal_width())
      return;
    *output_ << kMuted << label << suffix << kReset << std::flush;
    visible_ = true;
  }

  void redraw_thinking() { redraw(kThinkingFrames[frame_], kThinkingDots[dots_frame_]); }

  void animate() {
    std::unique_lock lock(mutex_);
    for (;;) {
      changed_.wait(lock, [this] { return stopping_ || thinking_; });
      if (stopping_)
        return;
      const auto sequence = thinking_sequence_;
      if (changed_.wait_for(lock, std::chrono::seconds(1), [this, sequence] {
            return stopping_ || !thinking_ || thinking_sequence_ != sequence;
          }))
        continue;
      dots_frame_ = (dots_frame_ + 1) % kThinkingDots.size();
      if (dots_frame_ == 0)
        frame_ = (frame_ + 1) % kThinkingFrames.size();
      redraw_thinking();
    }
  }

  static constexpr std::array<std::string_view, 4> kThinkingDots{"...", ".", "..", "..."};
  // Includes each entry with at least 50 million total speakers in Ethnologue's 2026 ranking.
  static constexpr auto kThinkingFrames = std::to_array<std::string_view>({
      "Thinking",           // English
      "Pensando",           // Spanish and Portuguese
      "正在思考",            // Mandarin Chinese
      "सोच रहा हूँ",           // Hindi
      "أفكّر",               // Modern Standard Arabic
      "Je réfléchis",       // French
      "ভাবছি",               // Bengali
      "Pondering",
      "Sedang berpikir",    // Indonesian
      "سوچ رہا ہوں",         // Urdu
      "Думаю",              // Russian
      "Denke nach",         // German
      "考え中",              // Japanese
      "I dey think",        // Nigerian Pidgin
      "بفكّر",               // Egyptian Arabic
      "Considering",
      "विचार करत आहे",       // Marathi
      "Đang suy nghĩ",      // Vietnamese
      "ఆలోచిస్తున్నాను",       // Telugu
      "Ninafikiria",         // Swahili
      "Ina tunani",         // Hausa
      "Düşünüyorum",        // Turkish
      "سوچ ریا واں",         // Western Punjabi
      "Mulling it over",
      "Nag-iisip",          // Tagalog
      "யோசிக்கிறேன்",         // Tamil
      "諗緊",                // Yue Chinese (Cantonese)
      "辣海想",              // Wu Chinese (Shanghainese)
      "دارم فکر می‌کنم",       // Iranian Persian
      "생각 중",             // Korean
      "እያሰብኩ ነው",            // Amharic
      "Working it out",
      "กำลังคิด",            // Thai
      "Lagi mikir",         // Javanese
      "Sto pensando",       // Italian
      "વિચારી રહ્યો છું",        // Gujarati
      "ಯೋಚಿಸುತ್ತಿದ್ದೇನೆ",        // Kannada
      "عم فكّر",             // Levantine Arabic
      "قاعد أفكّر",          // Sudanese Arabic
      "Mo ń ronú",          // Yoruba
      "सोचत बानी",           // Bhojpuri
  });
  std::ostream *output_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  std::size_t thinking_sequence_{};
  std::size_t frame_{};
  std::size_t dots_frame_{};
  bool thinking_{};
  bool visible_{};
  bool stopping_{};
};

std::string tool_label(std::string_view name) {
  auto label = detail::terminal_text(name);
  std::replace(label.begin(), label.end(), '_', ' ');
  return label;
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
  TerminalInput(std::istream &input, const TerminalHarnessOptions &options, bool terminal)
      : input_(input), options_(options), terminal_(terminal) {
#ifdef JUNO_AGENT_HAS_EDITLINE
    if (!terminal)
      return;

    prompt_ = options.prompt;
    auto *editor = el_init("juno", stdin, stdout, stderr);
    if (!editor)
      return;
    if (el_set(editor, EL_CLIENTDATA, this) == -1 ||
        el_set(editor, EL_PROMPT, &TerminalInput::prompt) == -1 ||
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
      if (!options_.prompt.empty()) {
        if (terminal_)
          write_colored(options_, kAccent, options_.prompt);
        else
          *output << options_.prompt;
      }
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
    constexpr std::array common{"/help", "/tools", "/activity", "/reasoning", "/history",
                                "/new",  "/quit",  "/clear",    "/exit"};
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
  bool terminal_;
};

} // namespace

TerminalHarness TerminalHarness::create(TerminalHarnessOptions options) {
  return TerminalHarness(std::move(options));
}

TerminalHarness::TerminalHarness(TerminalHarnessOptions options)
    : options_(std::move(options)), conversation_(options_.agent.start_conversation()) {}

Expected<RunResult> TerminalHarness::invoke(std::string_view prompt, std::stop_token stop_token) {
  return invoke(prompt, stop_token, terminal_output(options_));
}

Expected<RunResult> TerminalHarness::invoke(std::string_view prompt, std::stop_token stop_token,
                                            bool terminal) {
  auto *output = options_.output;
  const bool color = terminal && color_enabled(options_);
  StatusLine status(color ? output : nullptr, next_thinking_frame_);
  std::optional<detail::TerminalMarkdown> markdown;
  if (terminal)
    markdown.emplace(*output, color, std::min<std::size_t>(terminal_width(), 78));

  last_activity_.clear();
  last_model_times_.clear();
  std::optional<Clock::time_point> tool_started_at;
  std::size_t summarized = 0;
  bool line_open = false;
  bool streamed_text = false;
  bool writing_answer = false;
  const auto finish_line = [&] {
    if (line_open) {
      *output << '\n';
      line_open = false;
    }
  };
  const auto finish_answer = [&] {
    status.clear();
    if (markdown)
      markdown->finish();
    finish_line();
    writing_answer = false;
  };
  const auto summarize = [&] {
    if (!options_.show_tools || summarized == last_activity_.size())
      return;
    std::vector<std::pair<std::string_view, std::size_t>> counts;
    for (; summarized < last_activity_.size(); ++summarized) {
      const auto &activity = last_activity_[summarized];
      if (activity.error)
        continue;
      const auto name = std::string_view(activity.call.name);
      const auto entry = std::ranges::find(counts, name, &decltype(counts)::value_type::first);
      if (entry == counts.end())
        counts.emplace_back(name, 1);
      else
        ++entry->second;
    }
    if (counts.empty())
      return;
    std::string summary = "Tools";
    for (const auto &[name, count] : counts) {
      summary += terminal ? " · " : " | ";
      summary += tool_label(name);
      if (count > 1)
        summary += (terminal ? " ×" : " x") + std::to_string(count);
    }
    finish_line();
    write_colored(options_, kMuted, summary) << '\n';
    if (terminal)
      *output << '\n';
  };
  const auto write_answer = [&](std::string_view text) {
    if (text.empty() || (markdown && !writing_answer && trim(text).empty()))
      return;
    if (!writing_answer) {
      status.clear();
      finish_line();
      summarize();
      if (markdown) {
        for (auto newline = text.find('\n');
             newline != text.npos && trim(text.substr(0, newline)).empty();
             newline = text.find('\n')) {
          text.remove_prefix(newline + 1);
        }
        if (text.find('\n') == text.npos)
          status.show("Writing response...");
      }
    }
    streamed_text = true;
    writing_answer = true;
    if (markdown) {
      if (text.find('\n') != text.npos)
        status.clear();
      markdown->append(text);
    } else {
      *output << text << std::flush;
      line_open = text.back() != '\n';
    }
  };
  EventCallback render;
  if (output) {
    status.think();
    render = [&](const AgentEvent &event) {
      switch (event.type) {
      case EventType::TextDelta:
        write_answer(event.text);
        break;
      case EventType::ReasoningDelta:
        if (event.text.empty() || !options_.show_reasoning)
          break;
        status.clear();
        if (writing_answer)
          finish_answer();
        write_colored(options_, kMuted,
                      terminal ? detail::terminal_text(event.text, true) : event.text)
            << std::flush;
        line_open = event.text.back() != '\n';
        break;
      case EventType::ToolStarted:
        finish_answer();
        streamed_text = false;
        status.show(options_.show_tools ? "Running " + tool_label(event.tool_call.name) + "..."
                                        : "Working...");
        tool_started_at = Clock::now();
        break;
      case EventType::ToolCompleted:
        last_activity_.push_back({event.tool_call, event.tool_error,
                                  tool_started_at ? elapsed_since(*tool_started_at)
                                                  : Milliseconds{}});
        tool_started_at.reset();
        if (event.tool_error) {
          status.clear();
          write_colored(options_, kError, "Tool failed: ", tool_label(event.tool_call.name), " · ",
                        detail::terminal_text(*event.tool_error))
              << '\n' << std::flush;
        }
        status.think();
        break;
      case EventType::CompactionStarted:
        if (options_.compaction) {
          finish_answer();
          if (color)
            status.show("Compacting conversation...");
          else if (!terminal)
            *output << "Compacting conversation...\n" << std::flush;
        }
        break;
      case EventType::CompactionCompleted:
        if (options_.compaction) {
          status.clear();
          if (event.text != "no reduction")
            write_colored(options_, kMuted, "Conversation compacted.") << '\n' << std::flush;
          status.think();
        }
        break;
      default:
        break;
      }
    };
  }

  auto candidate = conversation_;
  const auto started_at = Clock::now();
  auto result = candidate.run(prompt, render, stop_token);
  const auto elapsed = elapsed_since(started_at);
  if (result && stop_token.stop_requested())
    result = Error{ErrorCode::Cancelled, "agent run was cancelled"};
  status.clear();
  if (color)
    next_thinking_frame_ = status.next_frame();
  if (output) {
    if (result) {
      last_model_times_ = result.value().model_generation_times;
      const auto &text = result.value().final_text;
      const bool blank = trim(text).empty();
      if (!streamed_text || blank) {
        finish_answer();
        write_answer(blank ? options_.empty_response : text);
      }
    }
    finish_answer();
    summarize();
    if (terminal && result && streamed_text) {
      *output << '\n';
      const auto model_elapsed =
          std::accumulate(last_model_times_.begin(), last_model_times_.end(), Milliseconds{});
      const auto passes = last_model_times_.size();
      auto timing = local_time_label();
      if (!timing.empty())
        timing += " · ";
      timing += format_elapsed(elapsed) + " total · " + format_elapsed(model_elapsed) + " model (" +
                std::to_string(passes) + (passes == 1 ? " pass)" : " passes)");
      if (!last_activity_.empty()) {
        Milliseconds tool_elapsed{};
        for (const auto &activity : last_activity_)
          tool_elapsed += activity.elapsed;
        timing += " · " + format_elapsed(tool_elapsed) + " tools";
      }
      write_colored(options_, kMuted, timing) << "\n\n";
    } else if (terminal && streamed_text) {
      *output << '\n';
    }
    *output << std::flush;
  }
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
  last_activity_.clear();
  last_model_times_.clear();
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
  last_activity_.clear();
  last_model_times_.clear();
  return {};
}

const std::optional<std::string> &TerminalHarness::active_session_id() const { return session_id_; }

// Run an interactive loop reading prompts and commands from the user.
Expected<void> TerminalHarness::run_interactive(std::istream &input) {
  // Print the optional banner once at the start of the interactive loop.
  auto *output = options_.output;
  const bool terminal = terminal_input(input) && terminal_output(options_);
  if (output && !options_.banner.empty())
    write_colored(options_, kTitle, options_.banner) << '\n';
  if (output && terminal)
    write_colored(options_, kMuted, "/help for commands") << "\n\n";
  else
    print_help();
  // Read lines from the terminal until EOF or a quit command is received.
  TerminalInput reader(input, options_, terminal);
  // The user can enter a prompt to invoke the agent, or a slash command to perform an action.
  for (std::string line;;) {
    auto read = reader.read(line);
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
    } else if (command == "/activity") {
      print_activity();
    } else if (name == "/reasoning") {
      if (argument.empty())
        options_.show_reasoning = !options_.show_reasoning;
      else if (argument == "on" || argument == "off")
        options_.show_reasoning = argument == "on";
      else {
        if (output)
          *output << "Usage: /reasoning [on|off]\n";
        continue;
      }
      if (output)
        *output << (options_.show_reasoning ? "Reasoning on.\n" : "Reasoning off.\n");
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
        *output << (terminal ? "\n" : "assistant:\n");
      auto result = invoke(line, {}, terminal);
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
      << " /help, /tools, /activity, /reasoning [on|off], /history, /new (/clear), /quit (/exit)";
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

void TerminalHarness::print_activity() const {
  auto *output = options_.output;
  if (!output)
    return;
  std::size_t pass = 0;
  for (const auto elapsed : last_model_times_)
    *output << "Model pass " << ++pass << "  " << format_elapsed(elapsed) << '\n';
  if (last_activity_.empty()) {
    *output << "No tool calls in the last turn.\n";
    return;
  }
  for (const auto &activity : last_activity_) {
    write_colored(options_, activity.error ? kError : kSuccess,
                  activity.error ? "Failed" : "Completed")
        << "  " << detail::terminal_text(activity.call.name) << '('
        << detail::terminal_text(activity.call.arguments_json) << ')';
    if (activity.error)
      *output << "  " << detail::terminal_text(*activity.error);
    *output << "  (" << format_elapsed(activity.elapsed) << ")\n";
  }
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
    write_colored(options_, kError, operation, ": ", detail::terminal_text(error.message)) << '\n';
}

} // namespace juno::sdk
