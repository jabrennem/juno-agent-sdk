#pragma once

#include <iostream>
#include <optional>

#include "juno_sdk/agent.hpp"
#include "juno_sdk/session_store.hpp"

namespace juno::sdk {

struct HarnessOptions {
  Agent agent;
  /** Borrowed terminal output stream. Set to nullptr to silence output. */
  std::ostream *output{&std::cout};
  bool show_tools{false};
  bool show_reasoning{false};
  std::string prompt{"You> "};
  std::string help;
  /** Enables /compact and /context with these manual compaction settings. */
  std::optional<CompactionOptions> compaction;
  /** Optional terminal message when the assistant returns only whitespace. */
  std::string empty_response;
  std::optional<SessionStore> session_store;
};

/** Synchronous terminal interface to one active conversation, used from a single thread. */
class Harness {
public:
  [[nodiscard]] static Harness create(HarnessOptions options);

  /** Streams a turn and returns its result. Failed turns leave the active transcript intact. */
  [[nodiscard]] Expected<RunResult> invoke(std::string_view prompt,
                                           std::stop_token stop_token = {});
  /** Reads prompts and commands until the user quits or input reaches EOF. */
  [[nodiscard]] Expected<void> run_interactive(std::istream &input = std::cin);

  void new_session();
  [[nodiscard]] Expected<void> resume_session(std::string_view id);
  [[nodiscard]] const std::optional<std::string> &active_session_id() const;

private:
  explicit Harness(HarnessOptions options);
  Expected<void> checkpoint(const Conversation &conversation);
  void print_help() const;
  void print_tools() const;
  void print_context() const;
  void compact();
  void print_sessions(std::string_view page) const;
  void print_recent_exchanges() const;
  void delete_session(std::string_view id);
  void report_error(std::string_view operation, const Error &error) const;

  HarnessOptions options_;
  Conversation conversation_;
  std::optional<std::string> session_id_;
};

} // namespace juno::sdk
