#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <stop_token>
#include <utility>

#include "juno_sdk/fake_model.hpp"
#include "juno_sdk/juno_sdk.hpp"
#include "../src/terminal_markdown.hpp"
#include "../src/terminal_thinking_animation.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {
using namespace juno::sdk;
namespace fs = std::filesystem;

class SessionDirectory {
public:
  SessionDirectory() {
    std::random_device random;
    do {
      path = fs::temp_directory_path() / ("juno-sessions-test-" + std::to_string(random()));
    } while (!fs::create_directory(path));
  }

  ~SessionDirectory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }

  fs::path path;
};

class CoutCapture {
public:
  CoutCapture() : previous_(std::cout.rdbuf(output.rdbuf())) {}
  ~CoutCapture() { std::cout.rdbuf(previous_); }

  std::ostringstream output;

private:
  std::streambuf *previous_;
};

TEST_CASE("Terminal thinking rotates through languages after four dot frames") {
  detail::TerminalThinkingAnimation animation;
  for (const char *label : {"Thinking", "Pensando", "正在思考", "सोच रहा हूँ", "أفكّر"}) {
    for (const char *suffix : {"", ".", "..", "..."}) {
      CHECK(std::string(animation.label()) == label);
      CHECK(std::string(animation.suffix()) == suffix);
      animation.advance();
    }
  }
}

TEST_CASE("Terminal thinking starts each word without dots when interrupted") {
  detail::TerminalThinkingAnimation animation;
  animation.advance();
  animation.restart_word();
  CHECK(std::string(animation.label()) == "Thinking");
  CHECK(animation.suffix().empty());

  detail::TerminalThinkingAnimation next_turn(animation.next_word());
  CHECK(std::string(next_turn.label()) == "Pensando");
  CHECK(next_turn.suffix().empty());
}

TEST_CASE("TerminalHarness invoke streams text and returns the turn result") {
  auto model = std::make_shared<FakeModel>(std::vector{
      FakeStep::final("Default"),
      FakeStep::final("Redirected"),
      FakeStep::final("Silent"),
  });
  auto agent = Agent::create({.model = model});
  CoutCapture console;

  auto harness = TerminalHarness::create({.agent = agent});
  const auto first = harness.invoke("Hello");
  REQUIRE(first);
  CHECK(first.value().final_text == "Default");
  CHECK(first.value().messages.size() == 2);
  const auto console_text = console.output.str();
  CHECK(console_text.find("Default") != std::string::npos);

  std::ostringstream redirected;
  auto custom = TerminalHarness::create({.agent = agent, .output = &redirected, .color = false});
  REQUIRE(custom.invoke("Hello"));
  CHECK(redirected.str() == "Redirected\n");

  auto silent = TerminalHarness::create({.agent = agent, .output = nullptr});
  const auto last = silent.invoke("Hello");
  REQUIRE(last);
  CHECK(last.value().final_text == "Silent");
  CHECK(console.output.str() == console_text);
}

TEST_CASE("TerminalHarness interactive commands share the invocation path") {
  auto model = std::make_shared<FakeModel>(std::vector{
      FakeStep::calls({{"call-1", "lookup", "{}"}}),
      FakeStep::final("Found it"),
      FakeStep::final("Fresh answer"),
  });
  auto agent = Agent::create({
      .model = model,
      .tools = {{{"lookup", "Look something up."},
                 [](std::string_view) -> Expected<std::string> { return "a result"; }}},
  });
  std::ostringstream output;
  auto harness = TerminalHarness::create({
      .agent = agent,
      .output = &output,
      .show_tools = true,
      .color = false,
      .banner = "Plain banner",
      .help = "Example-specific help.",
  });
  std::istringstream input("   \n/help\n/tools\nFirst prompt\n/history\n/clear\n/history\nNext "
                           "prompt\n/exit\nignored\n");
  REQUIRE(harness.run_interactive(input));
  CHECK(output.str().starts_with("Plain banner\n"));
  CHECK(output.str().find('\033') == std::string::npos);
  CHECK(output.str().find("Example-specific help.") != std::string::npos);
  CHECK(output.str().find("lookup: Look something up.") != std::string::npos);
  CHECK(output.str().find("Tools | lookup\nFound it") != std::string::npos);
  CHECK(output.str().find("4 messages") != std::string::npos);
  CHECK(output.str().find("0 messages") != std::string::npos);
  REQUIRE(model->requests().size() == 3);
  REQUIRE(model->requests().back().size() == 1);
  CHECK(model->requests().back().front().content == "Next prompt");
}

TEST_CASE("TerminalHarness can toggle reasoning without submitting a model prompt") {
  auto model = std::make_shared<FakeModel>(std::vector{
      FakeStep::final("First answer", "Hidden by default"),
      FakeStep::final("Second answer", "Visible reasoning"),
      FakeStep::final("Third answer", "Hidden again"),
  });
  std::ostringstream output;
  auto harness = TerminalHarness::create({
      .agent = Agent::create({.model = model}),
      .output = &output,
  });
  std::istringstream input("First prompt\n/reasoning\n/reasoning invalid\n"
                           "Second prompt\n/reasoning off\nThird prompt\n/exit\n");
  REQUIRE(harness.run_interactive(input));
  const auto text = output.str();
  CHECK(text.find("Reasoning on.\n") != std::string::npos);
  CHECK(text.find("Usage: /reasoning [on|off]\n") != std::string::npos);
  CHECK(text.find("Reasoning off.\n") != std::string::npos);
  CHECK(text.find("Visible reasoning\nSecond answer") != std::string::npos);
  CHECK(text.find("Hidden by default") == std::string::npos);
  CHECK(text.find("Hidden again") == std::string::npos);
  REQUIRE(model->requests().size() == 3);
  CHECK(model->requests()[1].back().content == "Second prompt");
}

TEST_CASE("TerminalHarness keeps scripted output plain") {
  auto model = std::make_shared<FakeModel>(std::vector{
      FakeStep::final("Hello"),
      FakeStep::failure({ErrorCode::GenerationFailed, "generation failed"}),
  });
  std::ostringstream output;
  auto harness = TerminalHarness::create({
      .agent = Agent::create({.model = model}),
      .output = &output,
      .prompt = "[test] > ",
      .banner = "Playground",
      .help = "Try a prompt.",
  });
  std::istringstream input("/help\nHi\nTry again\n/quit\n");

  REQUIRE(harness.run_interactive(input));
  const auto text = output.str();
  CHECK(text.starts_with("Playground\nTry a prompt.\nCommands: "));
  CHECK(text.find("[test] > assistant:\nHello\n") != std::string::npos);
  CHECK(text.find("Run failed: generation failed\n") != std::string::npos);
  CHECK(text.find('\033') == std::string::npos);
  CHECK(text.find("Playground", text.find("Playground") + 1) == std::string::npos);
}

TEST_CASE("TerminalHarness groups successful tool calls and exposes last-turn details") {
  auto tool_pass = FakeStep::calls(
      {{"call-1", "look_up", R"({"id":1})"}, {"call-2", "look_up", R"({"id":2})"},
       {"call-3", "missing", "{}"}}
  );
  tool_pass.reused_prompt_tokens = 24;
  auto answer_pass = FakeStep::final("**Found it**");
  answer_pass.reused_prompt_tokens = 72;
  auto model = std::make_shared<FakeModel>(std::vector{
      tool_pass,
      answer_pass,
  });
  auto agent = Agent::create({
      .model = model,
      .tools = {{{"look_up", "Look something up."},
                 [](std::string_view) -> Expected<std::string> {
                   return R"({"error":"a valid field in the returned data"})";
                 }}},
  });
  std::ostringstream output;
  auto harness = TerminalHarness::create({
      .agent = agent,
      .output = &output,
      .show_tools = true,
  });

  const auto result = harness.invoke("Look it up");
  REQUIRE(result);
  CHECK(result.value().final_text == "**Found it**");
  CHECK(result.value().model_generation_times.size() == 2);
  CHECK(result.value().reused_prompt_tokens == 96);
  const auto turn = output.str();
  CHECK(turn.find("Tool failed: missing") != std::string::npos);
  CHECK(turn.find("Tools | look up x2\n**Found it**\n") != std::string::npos);
  CHECK(turn.find("look_up(") == std::string::npos);
  CHECK(turn.find('\033') == std::string::npos);

  std::istringstream commands("/activity\n/new\n/activity\n/quit\n");
  REQUIRE(harness.run_interactive(commands));
  const auto transcript = output.str();
  CHECK(transcript.find("Model pass 1  ") != std::string::npos);
  CHECK(transcript.find("Model pass 2  ") != std::string::npos);
  CHECK(transcript.find(R"(Completed  look_up({"id":1}))") != std::string::npos);
  CHECK(transcript.find(R"(Completed  look_up({"id":2}))") != std::string::npos);
  CHECK(transcript.find("Failed  missing({})  unknown tool: missing  (") != std::string::npos);
  CHECK(transcript.find("No tool calls in the last turn.") != std::string::npos);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("TerminalHarness shows completed tools above the current step", "[tty]") {
  const auto *term = std::getenv("TERM");
  const auto *no_color = std::getenv("NO_COLOR");
  if (!isatty(STDOUT_FILENO) || (term && std::string_view(term) == "dumb") ||
      (no_color && *no_color != '\0'))
    SKIP("Requires a styled terminal");

  auto model = std::make_shared<FakeModel>(std::vector{
      FakeStep::calls({{"call-1", "lookup", "{}"}, {"call-2", "convert", "{}"}}),
      FakeStep::calls({{"call-3", "lookup", R"({"id":3})"}, {"call-4", "lookup", R"({"id":4})"}}),
      FakeStep::final("Found it\n"),
  });
  auto agent = Agent::create({
      .model = model,
      .tools = {{{"lookup", "Look something up."},
                 [](std::string_view) -> Expected<std::string> { return "result"; }},
                {{"convert", "Convert a value."},
                 [](std::string_view) -> Expected<std::string> { return "converted"; }}},
  });
  CoutCapture console;
  auto harness = TerminalHarness::create({.agent = agent, .show_tools = true});
  REQUIRE(harness.invoke("Look it up"));

  const auto text = console.output.str();
  const auto thinking = text.find("Thinking");
  const auto lookup = text.find("Running lookup...");
  const auto heading = text.find("Activity", lookup);
  const auto lookup_completed = text.find("- Tool: Completed lookup", heading);
  const auto after_lookup = text.find("Thinking", lookup_completed);
  const auto convert = text.find("Running convert...", after_lookup);
  const auto convert_completed = text.find("- Tool: Completed convert", convert);
  const auto repeated_lookup = text.find("Running lookup...", convert_completed);
  const auto after_tools = text.rfind("Thinking");
  const auto answer = text.find("Found it", after_tools);
  REQUIRE(answer != std::string::npos);
  CHECK(thinking < lookup);
  CHECK(repeated_lookup < after_tools);
  CHECK(text.find("Activity", heading + 1) == std::string::npos);
  CHECK(text.find("Tools ·") == std::string::npos);
  winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
      ((size.ws_col == 0 || size.ws_col >= 32) && (size.ws_row == 0 || size.ws_row >= 5))) {
    const auto grouped = text.find("- Tool: Completed lookup x3", repeated_lookup);
    CHECK(grouped < after_tools);
  }

  std::istringstream commands("/activity\n/quit\n");
  REQUIRE(harness.run_interactive(commands));
  const auto transcript = console.output.str();
  CHECK(transcript.find("Completed\033[0m  lookup({})") != std::string::npos);
  CHECK(transcript.find("Completed\033[0m  convert({})") != std::string::npos);
  CHECK(transcript.find(R"(lookup({"id":4}))") != std::string::npos);
}
#endif

TEST_CASE("Terminal Markdown formats split deltas and wraps lists while preserving code") {
  std::ostringstream output;
  detail::TerminalMarkdown markdown(output, true, 24);
  markdown.append("**Colum");
  CHECK(output.str().empty());
  markdown.append("bus, Ohio**\n- Bring a warm coat if you're going outside.\n");
  CHECK(output.str() == "\033[1mColumbus, Ohio\033[0m\n"
                        "• Bring a warm coat if\n"
                        "  you're going outside.\n");

  markdown.append("```text\n  **literal** text that should stay on one line\n");
  markdown.append("```\nRun `/activity`.");
  markdown.finish();
  CHECK(output.str().ends_with("  **literal** text that should stay on one line\n"
                               "Run \033[36m/activity\033[0m.\n"));
}

TEST_CASE("TerminalHarness keeps a checkpoint across compaction, failure, and restart") {
  const SessionDirectory directory;
  const auto store = SessionStore::create({.directory = directory.path});
  REQUIRE(store);
  auto model = std::make_shared<FakeModel>(std::vector{
      FakeStep::calls({{"old-call", "forecast", R"({"city":"Seattle"})"}}),
      FakeStep::final(std::string(350, 'a')),
      FakeStep::final("Today is sunny."),
      FakeStep::final("User asked for weather in Seattle."),
      FakeStep::failure({ErrorCode::GenerationFailed, "generation failed"}),
  });
  auto agent = Agent::create({
      .model = model,
      .system_prompt = "Old instructions.",
      .tools = {{{"forecast", "Look up the forecast."},
                 [](std::string_view) -> Expected<std::string> { return "sunny"; }}},
  });
  std::ostringstream output;
  auto harness = TerminalHarness::create({
      .agent = agent,
      .output = &output,
      .compaction = CompactionOptions{.preserve_recent_turns = 1},
      .session_store = store.value(),
  });
  REQUIRE(harness.invoke("Weather in Seattle?"));
  REQUIRE(harness.active_session_id());
  const auto id = *harness.active_session_id();
  CHECK(output.str().find("Tools |") == std::string::npos);
  REQUIRE(harness.invoke("What about today?"));
  std::istringstream commands("/context\n/compact\n/new\n/quit\n");
  REQUIRE(harness.run_interactive(commands));
  CHECK(output.str().find("Rendered prompt:") != std::string::npos);
  CHECK(output.str().find("Compacted model context") != std::string::npos);
  CHECK_FALSE(harness.active_session_id());
  REQUIRE(harness.resume_session(id));

  const auto original = store.value().load(id);
  REQUIRE(original);
  CHECK(original.value().info.title == "Weather in Seattle?");
  const auto &snapshot = original.value().snapshot;
  REQUIRE(snapshot.history.size() == 6);
  REQUIRE(snapshot.history[1].tool_calls.size() == 1);
  CHECK(snapshot.history[1].tool_calls.front().arguments_json == R"({"city":"Seattle"})");
  CHECK(snapshot.history[2].tool_call_id == "old-call");
  CHECK(snapshot.history[2].tool_name == "forecast");
  CHECK(snapshot.history[2].content == "sunny");
  REQUIRE(snapshot.context.size() == 3);
  CHECK(snapshot.context.front().content.find("Seattle") != std::string::npos);

  const auto failure = harness.invoke("Discard this failed turn");
  REQUIRE_FALSE(failure);
  CHECK(failure.error().code == ErrorCode::GenerationFailed);
  std::stop_source cancelled;
  cancelled.request_stop();
  const auto interrupted = harness.invoke("Discard this cancelled turn", cancelled.get_token());
  REQUIRE_FALSE(interrupted);
  CHECK(interrupted.error().code == ErrorCode::Cancelled);
  const auto after_failure = store.value().load(id);
  REQUIRE(after_failure);
  CHECK(after_failure.value().info.updated_at == original.value().info.updated_at);
  CHECK(after_failure.value().snapshot.history.size() == snapshot.history.size());

  const auto reopened_store = SessionStore::create({.directory = directory.path});
  REQUIRE(reopened_store);
  auto next_model = std::make_shared<FakeModel>(std::vector{
      FakeStep::calls({{"new-call", "forecast", R"({"city":"Seattle","days":2})"}}),
      FakeStep::final("Tomorrow is rainy."),
  });
  int forecast_calls = 0;
  auto reopened = TerminalHarness::create({
      .agent = Agent::create({
          .model = next_model,
          .system_prompt = "Current instructions.",
          .tools = {{{"forecast", "Fetch a fresh forecast."},
                     [&](std::string_view) -> Expected<std::string> {
                       ++forecast_calls;
                       return "rainy";
                     }}},
      }),
      .output = &output,
      .session_store = reopened_store.value(),
  });
  std::istringstream resume("/sessions\n/resume " + id + "\nWhat about tomorrow?\n");
  REQUIRE(reopened.run_interactive(resume));
  CHECK(output.str().find("user: What about today?") != std::string::npos);
  CHECK(reopened.active_session_id() == id);
  CHECK(forecast_calls == 1);
  REQUIRE(next_model->requests().size() == 2);
  const auto &request = next_model->requests().front();
  REQUIRE(request.size() == 4);
  CHECK(request.front().content.starts_with("Current instructions."));
  CHECK(request.front().content.find("Old instructions.") == std::string::npos);
  CHECK(request.front().content.find("Seattle") != std::string::npos);
  const auto updated = reopened_store.value().list();
  REQUIRE(updated);
  REQUIRE(updated.value().size() == 1);
  CHECK(updated.value().front().id == id);
  CHECK(updated.value().front().created_at == original.value().info.created_at);
  CHECK(updated.value().front().updated_at > original.value().info.updated_at);
}

TEST_CASE("TerminalHarness paginates and deletes saved sessions") {
  const SessionDirectory directory;
  const auto store = SessionStore::create({.directory = directory.path});
  REQUIRE(store);
  for (int i = 0; i < 21; ++i) {
    const Message message{Role::User, "Session " + std::to_string(i)};
    REQUIRE(store.value().save({{message}, {message}}));
  }
  const auto sessions = store.value().list();
  REQUIRE(sessions);
  const auto &oldest = sessions.value().back();
  auto model = std::make_shared<FakeModel>(std::vector<FakeStep>{});
  std::ostringstream output;
  auto harness = TerminalHarness::create({
      .agent = Agent::create({.model = model}),
      .output = &output,
      .session_store = store.value(),
  });
  std::istringstream first_page("/sessions\n");
  REQUIRE(harness.run_interactive(first_page));
  CHECK(output.str().find("page 1 of 2") != std::string::npos);
  CHECK(output.str().find(oldest.id) == std::string::npos);
  CHECK(output.str().find(sessions.value().front().id) != std::string::npos);
  std::istringstream second_page("/sessions 2\n/resume " + oldest.id + "\n/delete " + oldest.id);
  REQUIRE(harness.run_interactive(second_page));
  CHECK(output.str().find(oldest.id + "  " + oldest.title) != std::string::npos);
  CHECK_FALSE(harness.active_session_id());
  CHECK_FALSE(store.value().load(oldest.id));
  CHECK_FALSE(store.value().erase("../other"));
}

TEST_CASE("A failed session write leaves the previous checkpoint readable") {
  const SessionDirectory directory;
  const auto store = SessionStore::create({.directory = directory.path});
  REQUIRE(store);
  const Message first{Role::User, "First prompt"};
  const auto saved = store.value().save({{first}, {first}});
  REQUIRE(saved);
  const Message invalid{Role::Assistant, "\xFF"};
  CHECK_FALSE(store.value().save({{first, invalid}, {first, invalid}}, saved.value().id));
  const auto checkpoint = store.value().load(saved.value().id);
  REQUIRE(checkpoint);
  REQUIRE(checkpoint.value().snapshot.history.size() == 1);
  CHECK(checkpoint.value().snapshot.history.front().content == first.content);
}

} // namespace
