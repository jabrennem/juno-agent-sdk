#include <iostream>
#include <string>
#include <utility>

#include "juno_sdk/juno_sdk.hpp"

namespace
{

  using namespace juno::sdk;

  AgentOptions planning_options()
  {
    AgentOptions spec;
    spec.system_prompt = "You are SessionPrepPlanningAgent. Create an ordered "
                         "plan that turns raw multitrack WAV "
                         "files into a mix-ready session. Inspect both the "
                         "source files and the mix-session template. "
                         "Do not perform edits. Identify mappings, ambiguities, "
                         "and human decisions, then return a "
                         "numbered plan for SessionPrepAgent to execute.";
    spec.max_inference_turns = 6;
    spec.tools = {
        {{"inspect_audio_files", "Inspect the raw multitrack WAV file manifest.",
          R"({"type":"object","properties":{"folder":{"type":"string"}},"required":["folder"]})"},
         [](std::string_view) -> Expected<std::string>
         {
           return R"({"files":["Kick.wav","Snare.wav","Bass DI.wav","Rhythm L.wav","Rhythm R.wav","Lead Vocal.wav"],"sample_rate":48000,"bit_depth":24})";
         }},
        {{"inspect_mix_template", "Inspect the target mix-session template.",
          R"({"type":"object","properties":{"template":{"type":"string"}},"required":["template"]})"},
         [](std::string_view) -> Expected<std::string>
         {
           return R"({"folders":["Drums","Bass","Guitars","Vocals"],"buses":["Drum Bus","Music Bus","Vocal Bus"],"effects":["Plate","Delay"],"sample_rate":48000})";
         }},
    };
    return spec;
  }

  void print_help()
  {
    std::cout
        << "Describe the raw-files folder and mix template you want to plan "
           "from.\n"
        << "Example: Plan session prep for ./stems using ./templates/rock-mix.\n"
        << "Commands: /clear, /history, /tools, /help, /quit\n";
  }

  void print_tools(const Agent &agent)
  {
    const auto tools = agent.tool_definitions();
    if (tools.empty())
    {
      std::cout << "No tools configured.\n";
      return;
    }
    std::cout << "Available tools:\n";
    for (const auto &tool : tools)
      std::cout << "- " << tool.name << ": " << tool.description << '\n';
  }

} // namespace

int main(int argc, char **argv)
{
  if (argc != 2)
  {
    std::cerr << "Usage: " << argv[0] << " /path/to/model.gguf\n";
    return 2;
  }

  auto model = LlamaCppModel::create({.model_path = argv[1], .context_size = 4096});

  // Create an agent with the planning spec and start a conversation.
  auto options = planning_options();
  options.model = model;
  auto agent = Agent::create(std::move(options));
  auto conversation = agent.start_conversation();
  
  std::cout << "Session prep planning playground\n";
  print_help();

  // Main input loop for the planning agent.
  for (std::string input;
       std::cout << "[plan] > " && std::getline(std::cin, input);)
  {
    if (input == "/quit" || input == "/exit")
      break;
    if (input == "/help")
    {
      print_help();
      continue;
    }
    if (input == "/clear")
    {
      conversation.clear();
      std::cout << "Planning conversation cleared.\n";
      continue;
    }
    if (input == "/history")
    {
      std::cout << conversation.history().size() << " messages\n";
      continue;
    }
    if (input == "/tools")
    {
      print_tools(agent);
      continue;
    }
    if (input.empty())
      continue;

    std::cout << "assistant: ";
    auto result = conversation.run(input, [](const AgentEvent &event)
                                   {
      if (event.type == EventType::TextDelta)
        std::cout << event.text << std::flush; });
    if (!result)
      std::cerr << "\nRun failed: " << result.error().message << '\n';
    else
      std::cout << '\n';
  }
}
