# Repository Guide for Coding Agents

## Project overview

Juno Agent Runtime is a C++20 SDK for embedding a small tool-using agent loop in
native applications. The public API lives under `include/juno_sdk`, the
runtime implementation lives in `src`, and llama.cpp is an optional private
implementation dependency. Keep the SDK usable without exposing llama.cpp in
public headers. The JSON type used by the readable tool API is an intentional
public dependency.

## Build and test

Use the repository helper to build the SDK and run the test suite:

```sh
./build.sh --test
```

Add `--examples` when a change affects the Weather Agent example.

For a faster model-free test build, use:

```sh
JUNO_AGENT_BUILD_DIR=build-test ./build.sh --test \
  -DJUNO_AGENT_ENABLE_LLAMA_CPP=OFF
```

Useful environment variables are:

- `JUNO_AGENT_BUILD_DIR` to select a build directory.
- `JUNO_AGENT_BUILD_TYPE` to select the CMake build type.
- `CMAKE_BIN` and `CTEST_BIN` when those tools are not on `PATH`.

When changing installation, exported targets, or public headers, also verify a
consumer can use the installed package through `find_package(JunoAgentRuntime)`.

## Code organization

- `include/juno_sdk`: public SDK headers. Avoid leaking private model or
  third-party implementation details here; intentionally public dependencies
  must be documented and exported correctly.
- `src/agent.cpp`: conversation state, the inference loop, tool dispatch, and
  event delivery.
- `src/llama_cpp_model.cpp`: the in-process llama.cpp adapter.
- `test_support`: deterministic `FakeModel` support used only by tests.
- `tests/agent_tests.cpp`: Catch2 coverage of public agent behavior.
- `examples`: standalone playground applications; do not make core SDK behavior
  depend on them.
- `cmake`: installed package configuration.

## Implementation conventions

- Preserve C++20 compatibility and the `juno::sdk` namespace.
- Use RAII and value semantics. Prefer standard-library types in public APIs.
- Report expected failures with `Expected<T>` and `Error`; reserve exceptions
  for truly exceptional boundaries. Tool-handler exceptions must remain
  recoverable by the conversation loop.
- Keep `Agent` reusable. Each conversation receives an immutable snapshot of
  the agent configuration, owns an independent transcript, and is not
  thread-safe or reentrant.
- Preserve synchronous callback ordering and emit observable terminal errors.
- Tool arguments and results are JSON text. Handlers own argument validation;
  the SDK should not silently rewrite their payloads.
- Keep dependency linkage private unless a type is intentionally part of the
  installed API.
- Match the formatting and naming already used in neighboring code. Avoid
  drive-by formatting or unrelated cleanup.

## Public API design

The primary SDK API should use validated, options-based factory methods rather
than positional constructors or setter chains. Configuration belongs in named
aggregate `*Options` structs, and each class is responsible for validating its
own options before returning an object.

Use `PascalCase` for types and `snake_case` for methods, functions, and data
members. Follow the C++ standard-library naming convention consistently across
the public API.

The preferred shape is:

```cpp
auto model_result = LlamaCppModel::create({
    .model_path = "/path/to/model.gguf",
});

auto agent_result = Agent::create({
    .model = model_result.value(),
    .system_prompt = "You are a helpful assistant.",
});

auto agent = std::move(agent_result.value());
auto tool_result = Tool::create({
    .name = "get_labels",
    .description = "Return available track labels.",
    .handler = [](const JsonObject&) -> ToolResult {
      return {true, "[\\"drums\\",\\"bass\\",\\"vocals\\"]", {}};
    },
});
agent.add_tool(std::move(tool_result.value()));
auto conversation = agent.start_conversation();
auto result = conversation.run("Say hello.");
```

Factories should return `Expected<T>` when construction or validation can
fail. Mutating operations such as `add_tool` should return `Expected<void>`
when they can reject invalid state. Avoid positional arguments for public
configuration and avoid setter chains as the primary construction path.

Keep the conceptual model explicit:

- `Model` is the inference engine.
- `Agent` owns reusable behavior, prompt, tools, and options.
- `Conversation` owns one independent transcript and run state.
- `Tool` combines a name, description, parameter list, and callback.

The typed core (`AgentOptions`, `GenerationRequest`, `ToolDefinition`,
`Expected<T>`, and related types) remains available for advanced users and
internal correctness, but should not make the normal path feel ceremonial.
When adding capabilities, expose the options-based facade first and retain the
typed escape hatch. `Agent` should remain reusable: conversations receive an
immutable configuration snapshot, so tools added after a conversation starts
affect only subsequently created conversations.

## Tests

- Add or update Catch2 tests for behavior changes.
- Prefer `FakeModel` scripts for deterministic agent-loop tests; tests must not
  require a GGUF model or network inference.
- Cover both the successful path and relevant failures, including history and
  callback ordering when behavior is externally observable.
- Run the narrowest relevant build while iterating, then run `./build.sh --test` before
  handing off changes when dependencies are available.
- If full validation cannot run because dependency fetching or a local model is
  unavailable, report exactly what was and was not verified.

## Change discipline

- Read the relevant public headers, implementation, and tests before changing
  behavior.
- Preserve existing user changes in the working tree and avoid modifying
  unrelated files.
- Do not commit generated build output, downloaded dependencies, or GGUF model
  files.
- Treat `README.md` as the primary user guide. It should provide a clear path
  through project requirements, onboarding, building, usage, and contributing.
- Update `README.md` whenever a change affects that user journey, including
  public API usage, prerequisites, build options, examples, contribution
  workflows, or known limitations.
