# Weather Agent

Weather Agent takes a path to a local GGUF model. Its token limits are
set in [weather_agent.cpp](weather_agent.cpp). Run these commands from the repository root:

```sh
./build.sh --examples
./build/juno_weather_agent /absolute/path/to/model.gguf
```

Weather Agent enables [prompt cache retention](../../README.md#prompt-cache-retention) automatically.

Weather Agent streams formatted answers and adds completed tools to a live Activity
summary. Repeated calls update the tool's count. The status line beneath the summary
names the running tool and resumes the multilingual thinking animation when the tool
finishes.
Each reply ends with a local timestamp, timing, and the total prompt tokens reused
across the model passes for that reply. A count of zero means those passes reused
no prompt tokens.
Use `/activity` to inspect individual model pass times and tool arguments, outcomes, and durations.
Use `/reasoning` to toggle the display of reasoning emitted by the model during future turns.
Use `/reasoning on` or `/reasoning off` to set it explicitly.
Use `/help` to list commands. The agent keeps one recent turn when compacting.
Use `/compact` to compact on demand or `/context` to see token usage, model limits, and
the compaction threshold.

In a terminal, type `/` and press Tab to list commands, or type a prefix such as
`/hist` and press Tab to complete it. This uses the system libedit when available
(included with the macOS SDK and available as `libedit-dev` on Debian and Ubuntu).

Weather Agent saves conversations in `~/.juno/sessions/weather/`. After restarting, use `/sessions` to see the 20 most recently updated conversations and `/sessions 2` to browse older ones. Use `/resume ID` to see recent exchanges and continue under the same ID. Use `/new` to start fresh or `/delete ID` to remove a saved conversation.
[Local session recall #15](https://github.com/jabrennem/juno-agent-sdk/issues/15)

Tool-capable models need a compatible chat template. Juno Agent Runtime uses the model’s template by default. Set `LlamaCppOptions::chat_template_override` to supply a known compatible template name.
