#include <catch2/catch_test_macros.hpp>

#include <sstream>
#include <utility>

#include "juno_sdk/fake_model.hpp"
#include "juno_sdk/juno_sdk.hpp"

namespace {
using namespace juno::sdk;
class CoutCapture {
public:
  CoutCapture() : previous_(std::cout.rdbuf(output.rdbuf())) {}
  ~CoutCapture() { std::cout.rdbuf(previous_); }

  std::ostringstream output;

private:
  std::streambuf *previous_;
};

TEST_CASE("Harness invoke streams text and returns the turn result") {
  auto model = std::make_shared<FakeModel>(std::vector{
      FakeStep::final("Default"),
      FakeStep::final("Redirected"),
      FakeStep::final("Silent"),
  });
  auto agent = Agent::create({.model = model});
  CoutCapture console;

  auto harness = Harness::create({.agent = agent});
  const auto first = harness.invoke("Hello");
  REQUIRE(first);
  CHECK(first.value().final_text == "Default");
  CHECK(first.value().messages.size() == 2);
  CHECK(console.output.str() == "Default\n");

  std::ostringstream redirected;
  auto custom = Harness::create({.agent = agent, .output = &redirected});
  REQUIRE(custom.invoke("Hello"));
  CHECK(redirected.str() == "Redirected\n");

  auto silent = Harness::create({.agent = agent, .output = nullptr});
  const auto last = silent.invoke("Hello");
  REQUIRE(last);
  CHECK(last.value().final_text == "Silent");
  CHECK(console.output.str() == "Default\n");
}

TEST_CASE("Harness interactive commands share the invocation path") {
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
  auto harness = Harness::create({
      .agent = agent,
      .output = &output,
      .show_tools = true,
      .help = "Example-specific help.",
  });
  std::istringstream input("   \n/help\n/tools\nFirst prompt\n/history\n/clear\n/history\nNext "
                           "prompt\n/exit\nignored\n");
  REQUIRE(harness.run_interactive(input));
  CHECK(output.str().find("Example-specific help.") != std::string::npos);
  CHECK(output.str().find("lookup: Look something up.") != std::string::npos);
  CHECK(output.str().find("tool: lookup({})\ntool completed: lookup\nFound it") !=
        std::string::npos);
  CHECK(output.str().find("4 messages") != std::string::npos);
  CHECK(output.str().find("0 messages") != std::string::npos);
  REQUIRE(model->requests().size() == 3);
  REQUIRE(model->requests().back().size() == 1);
  CHECK(model->requests().back().front().content == "Next prompt");
}

} // namespace
