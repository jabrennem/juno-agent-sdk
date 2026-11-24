#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "juno_sdk/llama_cpp_model.hpp"

namespace {

using namespace juno::sdk;

const char *local_model_path() {
  const char *path = std::getenv("JUNO_TEST_GGUF");
  REQUIRE(path != nullptr);
  return path;
}

GenerationRequest request_for(const std::vector<Message> &messages) {
  return {
      .messages = messages,
      .config = {.max_tokens = 4, .temperature = 0.0F},
      .reasoning_effort = ReasoningEffort::None,
  };
}

} // namespace

TEST_CASE("Retained llama prompts match fresh generations after divergence and cancellation",
          "[.gguf][llama-cache]") {
  LlamaCppOptions options{.model_path = local_model_path()};
  const auto fresh = LlamaCppModel::create(options);
  options.prompt_cache_retention = PromptCacheRetention::MostRecentPrompt;
  const auto cached = LlamaCppModel::create(options);

  const std::vector<Message> parent{
      {Role::System, "Answer with a single city name."},
      {Role::User, "Which city is the capital of France?"},
  };
  auto follow_up = parent;
  follow_up.push_back({Role::Assistant, "Paris"});
  follow_up.push_back({Role::User, "Which city is the capital of Italy?"});
  const std::vector<Message> other{
      {Role::System, "Complete the following arithmetic problem with one number."},
      {Role::User, "What is two plus three?"},
  };

  const auto compare = [&](const std::vector<Message> &messages) {
    const auto request = request_for(messages);
    const auto expected = fresh->generate(request, {}, {});
    const auto actual = cached->generate(request, {}, {});
    REQUIRE(expected.has_value());
    REQUIRE(actual.has_value());
    REQUIRE_FALSE(expected.value().content.empty());
    CHECK(actual.value().content == expected.value().content);
    CHECK(actual.value().tool_calls.empty());
  };
  compare(parent);
  compare(parent); // Full match must still produce fresh logits.
  compare(follow_up);
  compare(parent); // Shortened model-facing history, as after compaction.
  compare(other);
  compare(parent);

  std::stop_source stop;
  const auto cancel_after_text = [&](const AgentEvent &event) {
    if (event.type == EventType::TextDelta)
      stop.request_stop();
  };
  const auto cancelled =
      cached->generate(request_for(follow_up), cancel_after_text, stop.get_token());
  REQUIRE_FALSE(cancelled.has_value());
  CHECK(cancelled.error().code == ErrorCode::Cancelled);
  compare(other);

  const auto throw_after_text = [](const AgentEvent &event) {
    if (event.type == EventType::TextDelta)
      throw std::runtime_error("interrupted stream");
  };
  REQUIRE_THROWS_AS(cached->generate(request_for(parent), throw_after_text, {}),
                    std::runtime_error);
  compare(follow_up);
}

TEST_CASE("Hybrid llama cache restores state for matching prompts", "[.gguf][llama-hybrid-cache]") {
  const auto model = LlamaCppModel::create({
      .model_path = local_model_path(),
      .prompt_cache_retention = PromptCacheRetention::MostRecentPrompt,
  });
  const std::vector<Message> messages{
      {Role::System, "Give a short answer about the weather. Use the supplied forecast details."},
      {Role::User, "In Columbus, the forecast calls for clear skies with a high of 43 degrees. "
                   "Will I need an umbrella today?"},
  };
  const auto first = model->generate(request_for(messages), {}, {});
  REQUIRE(first);
  CHECK(first.value().reused_prompt_tokens == 0);
  const auto repeated = model->generate(request_for(messages), {}, {});
  REQUIRE(repeated);
  CHECK(repeated.value().reused_prompt_tokens.value_or(0) > 0);
  CHECK(repeated.value().content == first.value().content);

  auto follow_up = messages;
  follow_up.push_back({Role::Assistant, "No, the forecast calls for clear skies."});
  follow_up.push_back({Role::User, "What about tomorrow?"});
  const auto continued = model->generate(request_for(follow_up), {}, {});
  REQUIRE(continued);
  CHECK(continued.value().reused_prompt_tokens.value_or(0) > 0);

  const std::vector<Message> other{{Role::User, "Say hello."}};
  const auto switched = model->generate(request_for(other), {}, {});
  REQUIRE(switched);
  CHECK(switched.value().reused_prompt_tokens == 0);
  const auto cold_follow_up = model->generate(request_for(follow_up), {}, {});
  REQUIRE(cold_follow_up);
  CHECK(cold_follow_up.value().reused_prompt_tokens == 0);
  CHECK(cold_follow_up.value().content == continued.value().content);
  const auto after_switch = model->generate(request_for(messages), {}, {});
  REQUIRE(after_switch);
  CHECK(after_switch.value().content == first.value().content);
}

TEST_CASE("Measure llama prompt cache time to first streamed token", "[.gguf][llama-cache-bench]") {
  const char *retention_env = std::getenv("JUNO_BENCH_RETENTION");
  const char *workload_env = std::getenv("JUNO_BENCH_WORKLOAD");
  const std::string_view retention = retention_env ? retention_env : "";
  const std::string_view workload = workload_env ? workload_env : "";
  REQUIRE((retention == "disabled" || retention == "recent"));
  REQUIRE((workload == "repeated" || workload == "alternating"));
  const auto model = LlamaCppModel::create({
      .model_path = local_model_path(),
      .prompt_cache_retention = retention == "recent" ? PromptCacheRetention::MostRecentPrompt
                                                      : PromptCacheRetention::Disabled,
  });

  // Synthetic weather tool round trips keep network and tool latency out of the measurement.
  std::vector<Message> histories[] = {
      {{Role::System, "Start with the city name. Summarize the Seattle forecast in a few words."}},
      {{Role::System, "Report the Miami weather in a few words, starting with the city name."}},
  };
  for (auto &history : histories) {
    history.push_back({Role::User, "What is the weather outlook this week?"});
    history.push_back({Role::Assistant, "I will check the local forecast."});
    std::string forecast;
    for (int hour = 0; hour < 36; ++hour) {
      forecast +=
          "Hour " + std::to_string(hour) + ": cloudy, 14 degrees Celsius, light southwest winds. ";
    }
    history.push_back({Role::Tool, std::move(forecast), {}, "forecast-1", "weather_forecast"});
  }

  using Clock = std::chrono::steady_clock;
  std::vector<double> follow_up_times;
  for (int turn = 0; turn <= 8; ++turn) {
    auto &history = histories[workload == "alternating" ? turn % 2 : 0];
    history.push_back(
        {Role::User, "Summarize the forecast starting at hour " + std::to_string(turn) + "."});
    const auto request = request_for(history);
    const auto metrics = model->measure(request);
    REQUIRE(metrics.has_value());
    const auto start = Clock::now();
    std::optional<double> first_token_ms;
    const auto response = model->generate(
        request,
        [&](const AgentEvent &event) {
          if (event.type == EventType::TextDelta && !first_token_ms) {
            first_token_ms =
                std::chrono::duration<double, std::milli>(Clock::now() - start).count();
          }
        },
        {});
    REQUIRE(response.has_value());
    REQUIRE(first_token_ms.has_value());
    std::cout << "pass " << turn + 1 << ", prompt tokens " << metrics.value().input_tokens
              << ", first streamed text " << *first_token_ms << " ms\n";
    if (turn > 0)
      follow_up_times.push_back(*first_token_ms);
    history.push_back({Role::Assistant, "Cloudy, with light southwest winds."});
  }
  std::sort(follow_up_times.begin(), follow_up_times.end());
  const auto middle = follow_up_times.size() / 2;
  std::cout << "Follow-up median: " << (follow_up_times[middle - 1] + follow_up_times[middle]) / 2
            << " ms\n";
}
