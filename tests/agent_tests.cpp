#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <stop_token>

#include "juno_sdk/fake_model.hpp"
#include "juno_sdk/juno_sdk.hpp"

namespace {
juno::sdk::Agent
make_agent(std::shared_ptr<juno::sdk::Model> model, juno::sdk::AgentOptions options = {}) {
  options.model = std::move(model);
  return juno::sdk::Agent::create(std::move(options));
}

class UserRequiredModel final : public juno::sdk::Model {
public:
  std::size_t context_size() const override {
    return 1024;
  }

  juno::sdk::Expected<juno::sdk::PromptMetrics>
  measure(const juno::sdk::GenerationRequest &request) override {
    const bool has_user_message = std::any_of(
        request.messages.begin(), request.messages.end(), [](const juno::sdk::Message &message) {
          return message.role == juno::sdk::Role::User;
        }
    );
    if (!has_user_message) {
      return juno::sdk::Error{
          juno::sdk::ErrorCode::GenerationFailed, "template requires a user message"
      };
    }
    return juno::sdk::PromptMetrics{42, true};
  }

  juno::sdk::Expected<juno::sdk::GenerationResponse> generate(
      const juno::sdk::GenerationRequest &,
      const juno::sdk::EventCallback &,
      std::stop_token
  ) override {
    return juno::sdk::GenerationResponse{"done", {}};
  }
};
} // namespace

TEST_CASE("Options factories validate SDK configuration") {
  REQUIRE_THROWS_AS(juno::sdk::Agent::create({}), juno::sdk::ConfigurationError);

  REQUIRE_THROWS_AS(
      juno::sdk::Tool::create({
          .name = "broken",
          .description = "A broken tool",
      }),
      juno::sdk::ConfigurationError
  );

  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{});
  REQUIRE_THROWS_AS(
      juno::sdk::Agent::create({
          .model = model,
          .compaction = {.max_context_percent = 101.0F},
      }),
      juno::sdk::ConfigurationError
  );
}

TEST_CASE("Options factories preserve conversation snapshots") {
  auto model = std::make_shared<juno::sdk::FakeModel>(
      std::vector<juno::sdk::FakeStep>{juno::sdk::FakeStep::final("done")}
  );
  auto agent = juno::sdk::Agent::create({
      .model = model,
      .system_prompt = "initial",
  });
  auto conversation = agent.start_conversation();
  REQUIRE_THROWS_AS(agent.add_tool({}), juno::sdk::ConfigurationError);
  CHECK(conversation.history().front().content == "initial");
}

TEST_CASE("Steering documents are composed once and shared by agents") {
  auto model = std::make_shared<juno::sdk::FakeModel>(
      std::vector<juno::sdk::FakeStep>{juno::sdk::FakeStep::final("done")}
  );
  auto steering = juno::sdk::Steering::create({
      .documents = {
          {.name = "first.md", .content = "first rules"},
          {.name = "second.md", .content = "second rules"},
      },
  });
  auto agent = juno::sdk::Agent::create({
      .model = model,
      .system_prompt = "base",
      .steering = steering,
  });

  const auto conversation = agent.start_conversation();
  REQUIRE(conversation.history().size() == 1);
  CHECK(
      conversation.history().front().content
      == "base\n\nSteering document: first.md\nfirst rules\n\nSteering document: second.md\nsecond "
         "rules"
  );

  auto second_agent = juno::sdk::Agent::create({
      .model = model,
      .system_prompt = "second base",
      .steering = steering,
  });
  const auto second_conversation = second_agent.start_conversation();
  CHECK(
      second_conversation.history().front().content
      == "second base\n\nSteering document: first.md\nfirst rules\n\nSteering document: second.md\n"
         "second rules"
  );
}

TEST_CASE("Steering configuration validates names and total size") {
  CHECK_THROWS_AS(
      juno::sdk::Steering::create({.documents = {{.name = "", .content = "rules"}}}),
      juno::sdk::ConfigurationError
  );
  CHECK_THROWS_AS(
      juno::sdk::Steering::create({
          .documents = {{.name = "rules.md", .content = "too large"}},
          .max_bytes = 3,
      }),
      juno::sdk::ConfigurationError
  );
}

TEST_CASE("Steering loads ordered documents from files") {
  const auto path = std::filesystem::temp_directory_path() / "juno-steering-test.md";
  {
    std::ofstream output(path);
    REQUIRE(output);
    output << "file rules";
  }

  auto steering = juno::sdk::Steering::create({
      .documents = {{.name = "inline.md", .content = "inline rules"}},
      .files = {path.string()},
  });

  REQUIRE(steering->documents().size() == 2);
  CHECK(steering->documents()[0].name == "inline.md");
  CHECK(steering->documents()[1].name == path.string());
  CHECK(steering->documents()[1].content == "file rules");
  std::filesystem::remove(path);
}

TEST_CASE("Steering rejects unreadable files") {
  CHECK_THROWS_AS(
      juno::sdk::Steering::create({.files = {"missing-steering-file.md"}}),
      juno::sdk::ConfigurationError
  );
}

TEST_CASE("A fake model returns a final response") {
  auto model = std::make_shared<juno::sdk::FakeModel>(
      std::vector<juno::sdk::FakeStep>{juno::sdk::FakeStep::final("hello")}
  );
  auto agent = make_agent(model);
  auto conversation = agent.start_conversation();

  auto result = conversation.run("hi");

  REQUIRE(result);
  CHECK(result.value().final_text == "hello");
  CHECK(result.value().inference_turns == 1);
  CHECK(conversation.history().size() == 2);
  REQUIRE(model->reasoning_effort_requests().size() == 1);
  CHECK(model->reasoning_effort_requests()[0] == juno::sdk::ReasoningEffort::Medium);
}

TEST_CASE("Manual compaction preserves the full transcript and reduces model context") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::final("one"),
      juno::sdk::FakeStep::final("two"),
      juno::sdk::FakeStep::final("three"),
      juno::sdk::FakeStep::final("summary"),
      juno::sdk::FakeStep::final("four")
  });
  auto agent = make_agent(model, {.system_prompt = "rules"});
  auto conversation = agent.start_conversation();

  REQUIRE(conversation.run(std::string(120, 'a')));
  REQUIRE(conversation.run(std::string(120, 'b')));
  REQUIRE(conversation.run(std::string(120, 'c')));
  const auto history_before = conversation.history().size();

  std::vector<juno::sdk::EventType> events;
  auto result =
      conversation.compact({.preserve_recent_turns = 1}, [&](const juno::sdk::AgentEvent &event) {
        events.push_back(event.type);
      });

  REQUIRE(result);
  CHECK(result.value().compacted);
  CHECK(conversation.history().size() == history_before);
  CHECK(model->requests().size() == 4);
  CHECK(
      events
      == std::vector<juno::sdk::EventType>{
          juno::sdk::EventType::CompactionStarted, juno::sdk::EventType::CompactionCompleted
      }
  );

  REQUIRE(conversation.run("fourth"));
  REQUIRE(model->requests().size() == 5);
  CHECK(model->requests().back().size() < history_before + 2);
  CHECK(
      model->requests().back().front().content.find("Earlier conversation was compacted")
      != std::string::npos
  );
}

TEST_CASE("Context usage exposes rendered prompt usage and capacity") {
  auto model = std::make_shared<juno::sdk::FakeModel>(
      std::vector<juno::sdk::FakeStep>{juno::sdk::FakeStep::final("done")}
  );
  auto agent = make_agent(model, {.system_prompt = "rules"});
  auto conversation = agent.start_conversation();

  const auto initial = conversation.context_usage();
  REQUIRE(initial);
  CHECK(initial.value().compaction_threshold == 3072);
  CHECK(initial.value().output_reservation == 512);
  CHECK(initial.value().context_capacity == 4096);
  CHECK(initial.value().message_count == 1);
  CHECK(initial.value().input_tokens > 0);
  CHECK(initial.value().exact);

  REQUIRE(conversation.run("hello"));
  const auto after = conversation.context_usage();
  REQUIRE(after);
  CHECK(after.value().message_count == 3);
}

TEST_CASE("Initial context usage supports templates that require a user message") {
  auto model = std::make_shared<UserRequiredModel>();
  auto conversation = make_agent(model, {.system_prompt = "rules"}).start_conversation();

  const auto usage = conversation.context_usage();

  REQUIRE(usage);
  CHECK(usage.value().input_tokens == 42);
  REQUIRE(conversation.history().size() == 1);
  CHECK(conversation.history().front().role == juno::sdk::Role::System);
}

TEST_CASE("Compaction is a no-op when there are no older turns to summarize") {
  auto model = std::make_shared<juno::sdk::FakeModel>(
      std::vector<juno::sdk::FakeStep>{juno::sdk::FakeStep::final("done")}
  );
  auto agent = make_agent(model, {.system_prompt = "rules"});
  auto conversation = agent.start_conversation();
  REQUIRE(conversation.run("hello"));

  const auto before = conversation.context_usage();
  REQUIRE(before);
  auto result = conversation.compact({.preserve_recent_turns = 4});

  REQUIRE(result);
  CHECK_FALSE(result.value().compacted);
  const auto after = conversation.context_usage();
  REQUIRE(after);
  CHECK(after.value().input_tokens == before.value().input_tokens);
  CHECK(model->requests().size() == 1);
}

TEST_CASE("Compaction leaves context unchanged when the summary is not smaller") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::final("one"),
      juno::sdk::FakeStep::final("two"),
      juno::sdk::FakeStep::final(std::string(800, 'x')),
  });
  auto agent = make_agent(model, {.system_prompt = "rules"});
  auto conversation = agent.start_conversation();

  REQUIRE(conversation.run("first"));
  REQUIRE(conversation.run("second"));
  const auto before = conversation.context_usage();
  REQUIRE(before);
  const auto history_before = conversation.history().size();

  auto result = conversation.compact({.preserve_recent_turns = 1});

  REQUIRE(result);
  CHECK_FALSE(result.value().compacted);
  CHECK(result.value().messages_before == before.value().message_count);
  CHECK(result.value().messages_after == before.value().message_count);
  CHECK(result.value().input_tokens_before == before.value().input_tokens);
  CHECK(result.value().input_tokens_after == before.value().input_tokens);
  const auto after = conversation.context_usage();
  REQUIRE(after);
  CHECK(after.value().input_tokens == before.value().input_tokens);
  CHECK(conversation.history().size() == history_before);
}

TEST_CASE("Automatic compaction runs before an oversized model request") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::final("one"),
      juno::sdk::FakeStep::final("summary"),
      juno::sdk::FakeStep::final("two")
  });
  auto agent =
      make_agent(model, {.compaction = {.max_context_tokens = 8, .preserve_recent_turns = 1}});
  auto conversation = agent.start_conversation();
  REQUIRE(conversation.run("a very long first message that exceeds the small budget"));

  std::vector<juno::sdk::EventType> events;
  REQUIRE(conversation.run("second", [&](const juno::sdk::AgentEvent &event) {
    events.push_back(event.type);
  }));
  CHECK(
      std::find(events.begin(), events.end(), juno::sdk::EventType::CompactionStarted)
      != events.end()
  );
  CHECK(model->requests().size() == 3);
}

TEST_CASE("Automatic compaction uses complete prompt measurement") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::final("one"),
      juno::sdk::FakeStep::final("summary"),
      juno::sdk::FakeStep::final("two")
  });
  model->set_prompt_overhead_tokens(97);
  auto agent = make_agent(
      model,
      {.generation = {.max_tokens = 16},
       .compaction = {.max_context_tokens = 100, .preserve_recent_turns = 1}}
  );
  auto conversation = agent.start_conversation();

  REQUIRE(conversation.run("first"));
  std::vector<juno::sdk::EventType> events;
  REQUIRE(conversation.run("second", [&](const juno::sdk::AgentEvent &event) {
    events.push_back(event.type);
  }));

  CHECK(
      std::find(events.begin(), events.end(), juno::sdk::EventType::CompactionStarted)
      != events.end()
  );
  CHECK(model->requests().size() == 3);
}

TEST_CASE("Context capacity is checked after compaction measurement") {
  auto model = std::make_shared<juno::sdk::FakeModel>(
      std::vector<juno::sdk::FakeStep>{juno::sdk::FakeStep::final("unused")}
  );
  model->set_context_size(100);
  model->set_prompt_overhead_tokens(85);
  auto agent = make_agent(
      model,
      {.generation = {.max_tokens = 20},
       .compaction = {.max_context_tokens = 80, .preserve_recent_turns = 1}}
  );

  auto result = agent.start_conversation().run("hello");

  REQUIRE_FALSE(result);
  CHECK(result.error().code == juno::sdk::ErrorCode::ContextLimitExceeded);
  CHECK(result.error().message.find("input tokens + 20 output tokens") != std::string::npos);
  CHECK(model->remaining_steps() == 1);
}

TEST_CASE("AgentOptions propagates reasoning effort to every inference turn") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "echo", "{}"}}),
      juno::sdk::FakeStep::final("done"),
  });
  auto agent = make_agent(
      model,
      {.reasoning_effort = juno::sdk::ReasoningEffort::Low,
       .tools = {
           {{"echo", "Echo input", "{}"},
            [](std::string_view) -> juno::sdk::Expected<std::string> { return std::string{"{}"}; }}
       }}
  );

  auto result = agent.start_conversation().run("test");

  REQUIRE(result);
  REQUIRE(model->reasoning_effort_requests().size() == 2);
  CHECK(model->reasoning_effort_requests()[0] == juno::sdk::ReasoningEffort::Low);
  CHECK(model->reasoning_effort_requests()[1] == juno::sdk::ReasoningEffort::Low);
}

TEST_CASE("The agent executes a tool and continues") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "echo", R"({"text":"hello"})"}}),
      juno::sdk::FakeStep::final("done"),
  });
  auto agent = make_agent(
      model,
      {.tools = {
           {{"echo", "Echo input", R"({"type":"object"})"},
            [](std::string_view arguments) -> juno::sdk::Expected<std::string> {
              return std::string(arguments);
            }}
       }}
  );
  auto conversation = agent.start_conversation();
  auto result = conversation.run("test");

  REQUIRE(result);
  CHECK(result.value().final_text == "done");
  REQUIRE(conversation.history().size() == 4);
  CHECK(conversation.history()[2].role == juno::sdk::Role::Tool);
  CHECK(conversation.history()[2].content == R"({"text":"hello"})");
}

TEST_CASE("A tool returns its own JSON result") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "get_labels", "{}"}}),
      juno::sdk::FakeStep::final("done"),
  });
  auto agent = make_agent(
      model,
      {.tools = {
           {{"get_labels", "Return the available track labels.", "{}"},
            [](std::string_view) -> juno::sdk::Expected<std::string> {
              return R"(["drums","bass","guitars","rhythm guitars","lead guitars","vocals"])";
            }}
       }}
  );

  auto result = agent.start_conversation().run("What labels are available?");

  REQUIRE(result);
  CHECK(result.value().messages[2].role == juno::sdk::Role::Tool);
  CHECK(
      result.value().messages[2].content
      == R"(["drums","bass","guitars","rhythm guitars","lead guitars","vocals"])"
  );
}

TEST_CASE("Tool results preserve their JSON text") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "labels", "{}"}}),
      juno::sdk::FakeStep::final("done"),
  });
  auto agent = make_agent(
      model,
      {.tools = {
           {{"labels", "Return labels.", "{}"},
            [](std::string_view) -> juno::sdk::Expected<std::string> {
              return R"(["quoted \"label\"","line\nbreak","back\\slash"])";
            }}
       }}
  );

  auto result = agent.start_conversation().run("Get labels");

  REQUIRE(result);
  CHECK(
      result.value().messages[2].content == R"(["quoted \"label\"","line\nbreak","back\\slash"])"
  );
}

TEST_CASE("Unknown tools become tool-result messages") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "missing", "{}"}}),
      juno::sdk::FakeStep::final("recovered"),
  });
  auto agent = make_agent(model);
  auto conversation = agent.start_conversation();
  auto result = conversation.run("test");

  REQUIRE(result);
  CHECK(conversation.history()[2].content.find("unknown tool") != std::string::npos);
}

TEST_CASE("Tool handlers own argument validation") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "echo", "not-json"}}),
      juno::sdk::FakeStep::final("recovered"),
  });
  bool invoked = false;
  auto agent = make_agent(
      model,
      {.tools = {
           {{"echo", "Echo input", "{}"},
            [&](std::string_view) -> juno::sdk::Expected<std::string> {
              invoked = true;
              return std::string{"{}"};
            }}
       }}
  );
  auto conversation = agent.start_conversation();
  auto result = conversation.run("test");

  REQUIRE(result);
  CHECK(invoked);
  CHECK(conversation.history()[2].content == "{}");
}

TEST_CASE("Iteration limits and callback ordering are observable") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "echo", "{}"}}),
      juno::sdk::FakeStep::final("unreachable"),
  });
  auto agent = make_agent(
      model,
      {.max_inference_turns = 1,
       .tools = {{{"echo", "Echo", "{}"}, [](std::string_view) -> juno::sdk::Expected<std::string> {
                    return std::string{"{}"};
                  }}}}
  );
  std::vector<juno::sdk::EventType> events;
  auto result = agent.start_conversation().run("test", [&](const juno::sdk::AgentEvent &event) {
    events.push_back(event.type);
  });

  REQUIRE_FALSE(result);
  CHECK(result.error().code == juno::sdk::ErrorCode::IterationLimitExceeded);
  REQUIRE(events.size() >= 3);
  CHECK(events[0] == juno::sdk::EventType::ToolStarted);
  CHECK(events[1] == juno::sdk::EventType::ToolCompleted);
  CHECK(events.back() == juno::sdk::EventType::Error);
}

TEST_CASE("Separate conversations retain separate histories") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::final("first"), juno::sdk::FakeStep::final("second")
  });
  auto agent = make_agent(model, {.system_prompt = "system"});
  auto one = agent.start_conversation();
  auto two = agent.start_conversation();
  REQUIRE(one.run("one"));
  REQUIRE(two.run("two"));
  CHECK(one.history()[1].content == "one");
  CHECK(two.history()[1].content == "two");
}

TEST_CASE("Thrown tool handlers become recoverable tool results") {
  auto model = std::make_shared<juno::sdk::FakeModel>(std::vector<juno::sdk::FakeStep>{
      juno::sdk::FakeStep::calls({{"call-1", "explode", "{}"}}),
      juno::sdk::FakeStep::final("recovered"),
  });
  auto agent = make_agent(
      model,
      {.tools = {
           {{"explode", "Always fails", "{}"},
            [](std::string_view) -> juno::sdk::Expected<std::string> {
              throw std::runtime_error("expected failure");
            }}
       }}
  );

  auto result = agent.start_conversation().run("test");

  REQUIRE(result);
  CHECK(
      result.value().messages[2].content.find("tool threw: expected failure") != std::string::npos
  );
}

TEST_CASE("A stopped run returns cancellation before mutating conversation history") {
  auto model = std::make_shared<juno::sdk::FakeModel>(
      std::vector<juno::sdk::FakeStep>{juno::sdk::FakeStep::final("unreachable")}
  );
  auto agent = make_agent(model);
  auto conversation = agent.start_conversation();
  std::stop_source stop_source;
  stop_source.request_stop();

  auto result = conversation.run("test", {}, stop_source.get_token());

  REQUIRE_FALSE(result);
  CHECK(result.error().code == juno::sdk::ErrorCode::Cancelled);
  CHECK(conversation.history().empty());
}
