#pragma once

#include <iostream>
#include <optional>

#include "juno_sdk/agent.hpp"
#include "juno_sdk/session_store.hpp"

namespace juno::sdk {

struct TerminalHarnessOptions {
  Agent agent;
  /** Borrowed terminal output stream. Set to nullptr to silence output. */
  std::ostream *output{&std::cout};
  /** Show the current tool and completed calls on styled terminals. Summarize calls in plain output. */
  bool show_tools{false};
  /** Initially display model reasoning deltas. Toggle during a session with /reasoning. */
  bool show_reasoning{false};
  /** Enable ANSI styling on supported terminals, subject to NO_COLOR. */
  bool color{true};
  std::string prompt{"You> "};
  /** Optional title printed once at the start of the interactive loop. */
  std::string banner;
  std::string help;
  /** Enables /compact and /context with these manual compaction settings. */
  std::optional<CompactionOptions> compaction;
  /** Optional terminal message when the assistant returns only whitespace. */
  std::string empty_response;
  std::optional<SessionStore> session_store;
};

/** Synchronous terminal interface to one active conversation, used from a single thread. */
class TerminalHarness {
public:
  [[nodiscard]] static TerminalHarness create(TerminalHarnessOptions options);

  /** Streams a turn and returns its result. Failed turns leave the active transcript intact. */
  [[nodiscard]] Expected<RunResult> invoke(std::string_view prompt,
                                           std::stop_token stop_token = {});
  /** Reads prompts and commands until the user quits or input reaches EOF. */
  [[nodiscard]] Expected<void> run_interactive(std::istream &input = std::cin);

  void new_session();
  [[nodiscard]] Expected<void> resume_session(std::string_view id);
  [[nodiscard]] const std::optional<std::string> &active_session_id() const;

private:
  struct ToolActivity {
    ToolCall call;
    std::optional<std::string> error;
    std::chrono::milliseconds elapsed;
  };

  explicit TerminalHarness(TerminalHarnessOptions options);
  Expected<RunResult> invoke(std::string_view prompt, std::stop_token stop_token, bool terminal);
  Expected<void> checkpoint(const Conversation &conversation);
  void print_help() const;
  void print_tools() const;
  void print_activity() const;
  void print_context() const;
  void compact();
  void print_sessions(std::string_view page) const;
  void print_recent_exchanges() const;
  void delete_session(std::string_view id);
  void report_error(std::string_view operation, const Error &error) const;

  TerminalHarnessOptions options_;
  Conversation conversation_;
  std::optional<std::string> session_id_;
  std::vector<ToolActivity> last_activity_;
  std::vector<std::chrono::milliseconds> last_model_times_;
  std::size_t next_thinking_frame_{};
};

} // namespace juno::sdk
