# juno-agent-runtime
A native modern C++ agent runtime for agents. Initially powered by llama.cpp.

Juno Agent Runtime is a C++20 SDK for embedding a small, tool-using agent loop in native applications in the Juno production agents ecosystem. 

It supports
- intuitive modern API design
- llama.cpp embedded integration for local gguf files
- tool integration
- Markdown-backed file memory

## Prerequisites

- CMake 3.20 or newer.
- A C++20 compiler: Apple Clang on macOS, or Clang/GCC on Linux.
- Git and network access when CMake is fetching dependencies.
- libcurl development files when building the weather playground.
- A GGUF model when using `LlamaCppModel`, including the llama.cpp playgrounds or a real-model smoke test.

On macOS, llama.cpp selects Metal support when it is available. Linux defaults to CPU; configure llama.cpp’s own CMake options in a parent build if you need CUDA, Vulkan, or another accelerator.

## Build

The simplest path is the repository build helper. It configures CMake, builds the library and playgrounds, and runs CTest:

```sh
./build.sh
```

The build does not need a model, but it does fetch and compile llama.cpp by default:

```sh
cmake -S . -B build -DJUNO_AGENT_BUILD_EXAMPLES=ON
cmake --build build
```

The llama.cpp model can also be enabled explicitly:

```sh
JUNO_AGENT_BUILD_DIR=build-llama ./build.sh -DJUNO_AGENT_ENABLE_LLAMA_CPP=ON
```

Useful CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `JUNO_AGENT_BUILD_TESTS` | `OFF` | Build the Catch2 unit test executable and register it with CTest. |
| `JUNO_AGENT_BUILD_EXAMPLES` | `ON` | Build the session-prep and weather playgrounds. |
| `JUNO_AGENT_ENABLE_LLAMA_CPP` | `ON` | Fetch and compile the in-process llama.cpp model. Set to `OFF` for a model-free build. |
| `JUNO_AGENT_FETCH_DEPS` | `ON` | Fetch pinned dependencies; set `OFF` to use installed packages. |
| `JUNO_AGENT_BUILD_LLAMA_SERVER` | `OFF` | Build llama.cpp's local HTTP server and tools when llama.cpp is enabled. |

`build.sh` defaults to an optimized Release build and accepts any additional CMake cache arguments. Set `JUNO_AGENT_BUILD_TYPE=Debug` when debugging, set `JUNO_AGENT_BUILD_DIR` to choose the build directory, and set `JUNO_AGENT_SKIP_TESTS=1` when you only want compilation. If CMake is installed outside your `PATH`, set `CMAKE_BIN=/path/to/cmake` (and `CTEST_BIN=/path/to/ctest`).

## Run the playgrounds

Each playground takes a path to a local GGUF model. The weather playground keeps its token limits
fixed in `examples/weather_playground.cpp`:

```sh
./build-llama/juno_session_prep_planning_playground /absolute/path/to/model.gguf
./build-llama/juno_weather_playground /absolute/path/to/model.gguf
```

The weather playground preserves one recent turn so compaction can be exercised quickly and supports `/compact` and `/context` in addition to the shared `/help`, `/clear`, `/history`, `/tools`, and `/quit` commands. The planning playground creates a session-preparation plan from raw WAV metadata and a mix template, with tools restricted to that domain. Its example tools simulate DAW operations and are intended to be replaced with calls into a production project service. Tool-capable models need a compatible chat template. Juno Agent Runtime uses the model’s template by default; `LlamaCppOptions::chat_template_override` can supply a known compatible template name.

## Unit tests

```sh
cmake -S . -B build-test -DJUNO_AGENT_BUILD_TESTS=ON
cmake --build build-test
ctest --test-dir build-test --output-on-failure
```

The automated tests link the build-only `juno_sdk_test_support` target, which provides `FakeModel`; they do not download a model. They cover final responses, tool loops, unknown tools, iteration limits, callbacks, handler-owned argument validation, and independent conversation histories.

## Use from another CMake application

Install Juno Agent Runtime SDK first:

```sh
cmake --install build --prefix /desired/prefix
```

Then consume its exported target:

```cmake
find_package(JunoAgentRuntime CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE Juno::Agent)
```

For a parent-project build, add the checkout through `FetchContent` instead:

```cmake
include(FetchContent)
FetchContent_Declare(juno_agent_runtime SOURCE_DIR /absolute/path/to/juno-agent-runtime)
FetchContent_MakeAvailable(juno_agent_runtime)
target_link_libraries(my_app PRIVATE Juno::Agent)
```

Minimal SDK use:

```cpp
#include <memory>
#include <string_view>
#include <utility>
#include "juno_sdk/juno_sdk.hpp"

auto model = juno::sdk::LlamaCppModel::create({.model_path = "/path/to/model.gguf"});
auto agent = juno::sdk::Agent::create({
    .model = model,
    .system_prompt = "Be concise.",
});
auto conversation = agent.start_conversation();
auto result = conversation.run("Say hello.", [](const juno::sdk::AgentEvent& event) {
  // TextDelta, ToolStarted, ToolCompleted, Completed, or Error.
});
```

User-authored steering can be supplied as ordered documents. A validated, immutable steering
object can be shared by many agents and conversations:

```cpp
auto steering = juno::sdk::Steering::create({
    .files = {"production-rules.md", "project-context.md"},
});

auto agent = juno::sdk::Agent::create({
    .model = model,
    .system_prompt = "Be concise.",
    .steering = steering,
});
```

Steering documents are ordered as provided, precomposed once, and limited to 32 KiB by default.
Use `files` for UTF-8 text files and `documents` for content supplied directly by the application.
Inline documents are composed before file-backed documents.

Tools are created and validated independently, and invalid options throw a typed exception:

```cpp
auto tool = juno::sdk::Tool::create({
    .name = "get_labels",
    .description = "Return available track labels.",
    .handler = [](const juno::sdk::JsonObject&) -> juno::sdk::ToolResult {
      return {true, R"(["drums","bass","vocals"])", {}};
    },
});
auto model = juno::sdk::LlamaCppModel::create({.model_path = "/path/to/model.gguf"});
auto agent = juno::sdk::Agent::create({.model = model});
agent.add_tool(std::move(tool));

auto conversation = agent.start_conversation();
auto result = conversation.run("Say hello.");
```

Reasoning effort is configured per agent and remains consistent across its
conversations and tool-loop turns:

```cpp
auto planner = juno::sdk::Agent::create({
    .model = model,
    .reasoning_effort = juno::sdk::ReasoningEffort::High});
auto task_doer = juno::sdk::Agent::create({
    .model = model,
    .reasoning_effort = juno::sdk::ReasoningEffort::Low});
```

`None` disables thinking for compatible templates. `Low`, `Medium`, and `High`
enable thinking and pass the corresponding effort level to templates that
support graded reasoning. Templates supporting only on/off reasoning treat all
non-`None` levels as enabled.

Tools can use either a JSON-object handler returning `ToolResult` or a string-based handler returning `Expected<std::string>`. Runtime tool-handler failures remain recoverable results, while invalid SDK configuration throws a typed exception. For example, a string-based handler can be supplied through `AgentOptions::tools`:

```cpp
auto options = juno::sdk::AgentOptions{
    .system_prompt = "Be concise.",
    .tools = {{{
        {"get_labels", "Return the available track labels.", R"({"type":"object"})"},
        [](std::string_view) -> juno::sdk::Expected<std::string> {
          return R"(["drums","bass","guitars","vocals"])");
        }}}};
auto agent = juno::sdk::Agent::create({
    .model = model,
    .system_prompt = options.system_prompt,
    .tools = std::move(options.tools),
});
auto conversation = agent.start_conversation();
```

Tool schemas, call arguments, and results use JSON strings. The same agent definition can create many independent conversations; each conversation owns its transcript and can be cleared without affecting the others.

## How it works

`Agent` owns a reusable model and shared behavior. Each `Conversation` owns an independent message history. A conversation sends its history to `Model::generate`, executes any returned tool calls, appends the tool results, and repeats until the model returns final text.

Long conversations can be compacted without changing the full transcript retained by the
application. Automatic compaction is enabled by default with a conservative 75% of the model's
configured runtime context as its rendered-input threshold. Hosts can use an absolute token
threshold instead or compact manually:

```cpp
auto agent = juno::sdk::Agent::create({
    .model = model,
    .compaction = {.max_context_percent = 75.0F, .preserve_recent_turns = 4},
});
auto conversation = agent.start_conversation();
auto result = conversation.compact({.preserve_recent_turns = 2,
                                    .focus = "Preserve decisions and unresolved failures."});
```

Compaction changes only the context sent to the model. `Conversation::history()` remains the
complete transcript, and system/steering context plus recent conversation turns are retained.
Before each inference turn, the model renders and measures the complete prompt, including chat
template and tool-protocol overhead. When that input reaches the threshold, Juno compacts it and
then renders and measures the replacement context again. The reserved output tokens are checked
separately against the model's hard context capacity.

`Conversation::context_usage()` returns the rendered input count, effective compaction threshold,
output reservation, context capacity, and message count. Backends that cannot count with their own
tokenizer may return an approximate measurement. Compaction lifecycle events are delivered through
the normal event callback.

```text
Conversation
  → build complete GenerationRequest
  → Model::measure
  → compact and remeasure when needed
  → verify input + output reservation fits
  → Model::generate
  → GenerationResponse
  → execute tool calls
  → append tool results
  → repeat until final text
```

`LlamaCppModel` adapts the request into a llama.cpp chat prompt, tokenizes it, generates tokens, streams text events, and parses the completed output.

Set `LlamaCppOptions::prompt_log_path` to append the exact post-template prompt sent to llama.cpp to a local diagnostic file. `LlamaCppOptions::batch_size` controls prompt decoding batch size; prompts larger than one batch are decoded in multiple chunks while remaining within `context_size`.

The model also emits an `EventType::Prompt` event immediately before tokenization. Its `text` field contains that same complete rendered prompt, allowing applications to inspect or print it through the normal event callback.

Juno uses a small JSON tool protocol rather than provider-native tool calling. Tool definitions are included in the prompt, and a model requests a tool by returning JSON in the `tool_calls` shape shown above. Tool results are rendered as ordinary conversation text so chat templates that do not support a native `tool` role can still render the exchange.

## Runtime model and limitations

- `Agent` holds a reusable model and configurable behavior; every `Conversation` receives an immutable configuration snapshot and owns an in-memory transcript.
- Conversations are not reentrant or thread-safe. Use one conversation per concurrent chat.
- Calls and event callbacks run synchronously, in model order.
- Tool errors—including unknown tools and handler exceptions—are appended as tool-result messages so a model can recover on its next turn.
- The built-in memory store exposes constrained Markdown file operations inside its configured directory. Agents are instructed to persist stable user-provided facts and preferences, while avoiding transient task data. It is not a semantic or multi-process memory system.
- `LlamaCppModel` is in-process and uses RAII to manage the model and per-run inference contexts. Its llama.cpp dependency is isolated from the SDK’s public headers.
- API-key/cloud models are not implemented yet; implement `Model` to add one without changing `Agent` or `Conversation`.

## Layout

| Path | Responsibility |
| --- | --- |
| `include/juno_sdk` | Public SDK API: runtime, messages, tools, errors, and production models. |
| `src/agent.cpp` | Agent loop, conversation transcript, tool dispatch, and events. |
| `test_support/fake_model.cpp` | Deterministic scripted model for build-only tests. |
| `test_support/juno_sdk/fake_model.hpp` | Build-only fake model API for tests. |
| `src/llama_cpp_model.cpp` | Private direct llama.cpp adapter. |
| `examples` | Independent session-prep planning and weather playground executables. |
| `tests` | Fake-model unit tests. |
