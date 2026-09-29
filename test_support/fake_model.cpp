#include "juno_sdk/fake_model.hpp"

#include <utility>

namespace juno::sdk {

FakeStep FakeStep::final(std::string text) {
  FakeStep step;
  step.kind = Kind::Final;
  step.content = std::move(text);
  return step;
}

FakeStep FakeStep::calls(std::vector<ToolCall> calls, std::string text) {
  FakeStep step;
  step.kind = Kind::ToolCalls;
  step.content = std::move(text);
  step.tool_calls = std::move(calls);
  return step;
}

FakeStep FakeStep::failure(Error error) {
  FakeStep step;
  step.kind = Kind::Failure;
  step.error = std::move(error);
  return step;
}

FakeModel::FakeModel(std::vector<FakeStep> script) : script_(std::move(script)) {}

Expected<PromptMetrics> FakeModel::measure(const GenerationRequest &request) {
  auto measured = Model::measure(request);
  if (!measured)
    return measured;
  measured.value().input_tokens += prompt_overhead_tokens_;
  measured.value().exact = true;
  return measured;
}

Expected<GenerationResponse> FakeModel::generate(
    const GenerationRequest &request,
    const EventCallback &callback,
    std::stop_token stop_token
) {
  if (stop_token.stop_requested()) {
    return make_unexpected(Error{ErrorCode::Cancelled, "generation was cancelled"});
  }
  if (next_step_ == script_.size()) {
    return make_unexpected(Error{ErrorCode::GenerationFailed, "fake model script is exhausted"});
  }
  reasoning_effort_requests_.push_back(request.reasoning_effort);
  requests_.emplace_back(request.messages.begin(), request.messages.end());

  const FakeStep &step = script_[next_step_++];
  if (step.kind == FakeStep::Kind::Failure) {
    return make_unexpected(step.error);
  }
  if (!step.content.empty() && callback) {
    callback(AgentEvent{EventType::TextDelta, step.content, {}});
  }
  return GenerationResponse{step.content, step.tool_calls};
}

std::size_t FakeModel::remaining_steps() const {
  return script_.size() - next_step_;
}

const std::vector<ReasoningEffort> &FakeModel::reasoning_effort_requests() const {
  return reasoning_effort_requests_;
}

const std::vector<std::vector<Message>> &FakeModel::requests() const {
  return requests_;
}

void FakeModel::set_context_size(const std::size_t context_size) {
  context_size_ = context_size;
}

void FakeModel::set_prompt_overhead_tokens(const std::size_t prompt_overhead_tokens) {
  prompt_overhead_tokens_ = prompt_overhead_tokens;
}

} // namespace juno::sdk
