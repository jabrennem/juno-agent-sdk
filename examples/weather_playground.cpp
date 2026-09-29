#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <curl/curl.h>

#include "juno_sdk/juno_sdk.hpp"

namespace {

constexpr std::string_view kCyan = "\033[36m";
constexpr std::string_view kYellow = "\033[33m";
constexpr std::string_view kBlue = "\033[34m";
constexpr std::string_view kMagenta = "\033[35m";
constexpr std::string_view kReset = "\033[0m";
constexpr std::size_t kContextSize = 4096;
constexpr std::size_t kGenerationTokens = 512;

// Define structures to hold location and weather forecast data

struct Location {
  std::string city;
  double latitude;
  double longitude;
  std::string timezone;
};

struct WeatherForecast {
  Location location;
  double current_temperature;
  double high_temperature;
  double low_temperature;
  int weather_code;
};

// Helper function to write the response data from libcurl into a string
size_t write_response(char *data, size_t size, size_t count, void *user_data) {
  auto *response = static_cast<std::string *>(user_data);
  response->append(data, size * count);
  return size * count;
}

// Fetch JSON data from a given URL using libcurl
juno::sdk::Expected<juno::sdk::JsonObject> get_json(const std::string &url) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return juno::sdk::Error{
        juno::sdk::ErrorCode::ToolExecutionFailed, "could not initialize HTTP client"
    };

  std::string response;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK)
    return juno::sdk::Error{
        juno::sdk::ErrorCode::ToolExecutionFailed,
        std::string{"weather request failed: "} + curl_easy_strerror(result)
    };
  if (status < 200 || status >= 300)
    return juno::sdk::Error{
        juno::sdk::ErrorCode::ToolExecutionFailed,
        "weather service returned HTTP " + std::to_string(status)
    };

  try {
    return juno::sdk::JsonObject::parse(response);
  } catch (const juno::sdk::JsonObject::exception &error) {
    return juno::sdk::Error{
        juno::sdk::ErrorCode::ToolExecutionFailed,
        std::string{"weather service returned invalid JSON: "} + error.what()
    };
  }
}

// Geocode a city name to get its latitude, longitude, and timezone
juno::sdk::Expected<Location> geocode_city(const std::string &city) {
  CURL *curl = curl_easy_init();
  if (!curl)
    return juno::sdk::Error{
        juno::sdk::ErrorCode::ToolExecutionFailed, "could not initialize HTTP client"
    };

  char *encoded = curl_easy_escape(curl, city.c_str(), 0);
  if (!encoded) {
    curl_easy_cleanup(curl);
    return juno::sdk::Error{
        juno::sdk::ErrorCode::ToolExecutionFailed, "could not encode city name"
    };
  }
  const std::string url = "https://geocoding-api.open-meteo.com/v1/search?name="
                          + std::string(encoded) + "&count=1&language=en&format=json";
  curl_free(encoded);
  curl_easy_cleanup(curl);

  auto result = get_json(url);
  if (!result)
    return result.error();
  if (!result.value().contains("results") || result.value()["results"].empty())
    return juno::sdk::Error{juno::sdk::ErrorCode::ToolExecutionFailed, "could not find that city"};

  const auto &match = result.value()["results"].front();
  return Location{
      .city = match.value("name", city),
      .latitude = match.value("latitude", 0.0),
      .longitude = match.value("longitude", 0.0),
      .timezone = match.value("timezone", "auto")
  };
}

// Fetch the weather forecast for a given location and number of days
juno::sdk::Expected<WeatherForecast> fetch_forecast(const Location &location, int days) {
  std::ostringstream url;
  url << "https://api.open-meteo.com/v1/forecast?latitude=" << location.latitude
      << "&longitude=" << location.longitude << "&current=temperature_2m,weather_code"
      << "&daily=temperature_2m_max,temperature_2m_min"
      << "&forecast_days=" << days << "&timezone=auto";

  auto result = get_json(url.str());
  if (!result)
    return result.error();
  try {
    const auto &current = result.value().at("current");
    const auto &daily = result.value().at("daily");
    return WeatherForecast{
        .location = location,
        .current_temperature = current.at("temperature_2m"),
        .high_temperature = daily.at("temperature_2m_max").at(0),
        .low_temperature = daily.at("temperature_2m_min").at(0),
        .weather_code = current.at("weather_code")
    };
  } catch (const juno::sdk::JsonObject::exception &error) {
    return juno::sdk::Error{
        juno::sdk::ErrorCode::ToolExecutionFailed,
        std::string{"weather response was missing expected data: "} + error.what()
    };
  }
}

std::string format_forecast(const WeatherForecast &forecast) {
  std::ostringstream output;
  output << "Weather in " << forecast.location.city << " (" << forecast.location.timezone
         << "): currently " << forecast.current_temperature << " degrees, with a high of "
         << forecast.high_temperature << " and a low of " << forecast.low_temperature
         << " today. Weather code " << forecast.weather_code << ". Data from Open-Meteo.";
  return output.str();
}

bool is_blank(const std::string &text) {
  return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::string format_token_count(const std::size_t value) {
  std::string result = std::to_string(value);
  for (std::size_t offset = result.size(); offset > 3; offset -= 3)
    result.insert(offset - 3, ",");
  return result;
}

std::string
format_threshold_usage(const std::size_t input_tokens, const std::size_t compaction_threshold) {
  if (compaction_threshold == 0)
    return "automatic compaction disabled";
  std::ostringstream output;
  output << std::fixed << std::setprecision(1)
         << (100.0 * static_cast<double>(input_tokens) / static_cast<double>(compaction_threshold))
         << "% of compaction threshold";
  return output.str();
}

void print_help() {
  std::cout << "Ask for a weather forecast for any city.\n"
            << "Example: What's the weather in Seattle for five days?\n"
            << "Commands: /clear, /compact, /context, /history, /tools, /help, /quit\n"
            << "Automatic compaction: enabled at 75% of the model context budget.\n";
}

void print_tools(const juno::sdk::Agent &agent) {
  const auto tools = agent.tool_definitions();
  if (tools.empty()) {
    std::cout << "No tools configured.\n";
    return;
  }
  std::cout << "Available tools:\n";
  for (const auto &tool : tools)
    std::cout << "- " << tool.name << ": " << tool.description << '\n';
}

// Callback function for the temperature_unit_conversion tool
juno::sdk::ToolResult conversion_tool_handler(const juno::sdk::JsonObject &params) {
  if (!params.contains("temperature_value") || !params["temperature_value"].is_number())
    return {false, "temperature_value must be a number."};
  if (!params.contains("source_unit") || !params["source_unit"].is_string())
    return {false, "source_unit must be either 'celsius' or 'fahrenheit'."};

  const double temperature_value = params["temperature_value"].get<double>();
  const std::string source_unit = params["source_unit"].get<std::string>();

  double result = 0.0;
  std::string target_unit;
  if (source_unit == "celsius") {
    result = temperature_value * (9.0 / 5.0) + 32.0;
    target_unit = "Fahrenheit";
  } else if (source_unit == "fahrenheit") {
    result = (temperature_value - 32.0) * (5.0 / 9.0);
    target_unit = "Celsius";
  } else {
    return {false, "source_unit must be either 'celsius' or 'fahrenheit'."};
  }

  std::ostringstream output;
  output << "Converted temperature: " << result << ' ' << target_unit;
  return {true, output.str()};
}

// Callback function for the weather_forecast tool
juno::sdk::ToolResult weather_forecast_tool_handler(const juno::sdk::JsonObject &params) {
  // Determine the city to fetch the forecast for
  std::string city;
  if (params.contains("city") && params["city"].is_string())
    city = params["city"].get<std::string>();
  else if (params.contains("city"))
    return {false, "weather_forecast requires city to be a string"};

  if (city.empty())
    return {false, "Please provide a city."};

  // Determine the number of days for the forecast
  const int days = params.value("days", 1);
  if (days < 1 || days > 16)
    return {false, "weather_forecast supports between 1 and 16 days"};

  // Geocode the city to get its location
  auto geocoded = geocode_city(city);
  if (!geocoded)
    return {false, geocoded.error().message};
  const auto location = geocoded.value();

  // Fetch the weather forecast for the location
  auto forecast = fetch_forecast(location, days);
  if (!forecast)
    return {false, forecast.error().message};

  // Return the formatted forecast
  return {true, format_forecast(forecast.value())};
}

} // namespace

int main(int argc, char **argv) {

  // Initialize libcurl for HTTP requests
  curl_global_init(CURL_GLOBAL_DEFAULT);
  const auto curl_cleanup = [] { curl_global_cleanup(); };

  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " /path/to/model.gguf\n";
    curl_cleanup();
    return 2;
  }

  // Load the LLaMA.cpp model
  auto model = juno::sdk::LlamaCppModel::create({
      .model_path = argv[1],
      .context_size = kContextSize,
  });

  // Keep durable weather preferences and recurring locations in an inspectable Markdown store.
  auto recent_forecasts_store = juno::sdk::MemoryStore::create({
      .name = "weather",
      .path = "~/.juno/memory/weather",
      .description = "Recent cities, weather preferences, and recurring weather requests.",
  });
  auto weather_memory = juno::sdk::MemoryManager::create({
      .stores = {recent_forecasts_store},
  });

  // Create a tool for fetching the weather forecast
  auto forecast_tool = juno::sdk::Tool::create(
      {.name = "weather_forecast",
       .description = "Get weather forecast for a city.",
       .parameters =
           {{.name = "city",
             .description = "The city to forecast",
             .type = "string",
             .required = true},
            {.name = "days",
             .description = "Number of days for the forecast",
             .type = "integer",
             .required = false}},
       .handler = weather_forecast_tool_handler}
  );

  auto conversion_tool = juno::sdk::Tool::create(
      {.name = "temperature_unit_conversion",
       .description = "Convert a temperature between Celsius and Fahrenheit. Always use this tool "
                      "instead of calculating a temperature conversion yourself.",
       .parameters =
           {
               {.name = "temperature_value",
                .description = "The numeric temperature to convert",
                .type = "number",
                .required = true},
               {.name = "source_unit",
                .description = "The current unit: 'celsius' or 'fahrenheit'",
                .type = "string",
                .required = true},
           },
       .handler = conversion_tool_handler}
  );

  auto steering = juno::sdk::Steering::create({
      .files = {"examples/weather-steering.md"},
  });

  // Create an agent that uses the model and the weather forecast tool
  auto weather_agent = juno::sdk::Agent::create({
      .model = model,
      .system_prompt =
          "You are WeatherAgent. Answer weather questions concisely using the available tools "
          "and durable memory when useful. For every Celsius/Fahrenheit conversion, always call "
          "temperature_unit_conversion; never calculate the conversion yourself.",
      .steering = steering,
      .generation = {.max_tokens = kGenerationTokens},
      .reasoning_effort = juno::sdk::ReasoningEffort::Low,
      .max_inference_turns = 6,
      .compaction = {.max_context_percent = 75.0F, .preserve_recent_turns = 1},
      .tools = {forecast_tool, conversion_tool},
      .memory = weather_memory,
  });
  auto conversation = weather_agent.start_conversation();

  // Start the interactive playground
  std::cout << "Weather playground (" << kContextSize << " context tokens, " << kGenerationTokens
            << " generation tokens)\n";
  print_help();

  for (std::string input; std::cout << "[weather] > " && std::getline(std::cin, input);) {
    if (input == "/quit" || input == "/exit")
      break;
    if (input == "/help") {
      print_help();
      continue;
    }
    if (input == "/clear") {
      conversation.clear();
      std::cout << "Weather conversation cleared.\n";
      continue;
    }
    if (input == "/compact") {
      auto result = conversation.compact(
          {.preserve_recent_turns = 1,
           .focus = "Preserve user preferences and recent weather context."},
          [&](const juno::sdk::AgentEvent &event) {
            if (event.type == juno::sdk::EventType::CompactionStarted)
              std::cout << "Compacting conversation...\n";
          }
      );
      if (!result) {
        std::cout << "Compaction failed: " << result.error().message << '\n';
      } else if (!result.value().compacted) {
        std::cout << "Compaction made no reduction; model context was left unchanged.\n";
      } else {
        std::cout << "Compacted model context from " << result.value().messages_before << " to "
                  << result.value().messages_after << " messages ("
                  << result.value().input_tokens_before << " to "
                  << result.value().input_tokens_after
                  << " rendered prompt tokens). Full history is preserved.\n";
      }
      continue;
    }
    if (input == "/context") {
      const auto usage = conversation.context_usage();
      if (!usage) {
        std::cout << "Could not measure context: " << usage.error().message << '\n';
      } else {
        std::cout << "Rendered prompt: " << format_token_count(usage.value().input_tokens)
                  << " tokens ("
                  << format_threshold_usage(
                         usage.value().input_tokens, usage.value().compaction_threshold
                     )
                  << ")\n"
                  << "Compaction threshold: "
                  << format_token_count(usage.value().compaction_threshold) << " tokens\n"
                  << "Output reservation: " << format_token_count(usage.value().output_reservation)
                  << " tokens\n"
                  << "Context capacity: " << format_token_count(usage.value().context_capacity)
                  << " tokens\n";
      }
      continue;
    }
    if (input == "/history") {
      std::cout << conversation.history().size() << " messages\n";
      continue;
    }
    if (input == "/tools") {
      print_tools(weather_agent);
      continue;
    }
    if (input.empty())
      continue;

    std::cout << "assistant:\n";
    std::string forecast_fallback;
    auto result = conversation.run(input, [&](const juno::sdk::AgentEvent &event) {
      if (event.type == juno::sdk::EventType::Prompt) {
        // std::cout << kMagenta << event.text << kReset << std::flush;
      } else if (event.type == juno::sdk::EventType::ReasoningDelta) {
        std::cout << kCyan << event.text << kReset << std::flush;
      } else if (event.type == juno::sdk::EventType::TextDelta) {
        std::cout << kBlue << event.text << kReset << std::flush;
      } else if (event.type == juno::sdk::EventType::ToolStarted) {
        std::cout << kYellow << "tool: " << event.tool_call.name << "("
                  << event.tool_call.arguments_json << ")" << kReset << '\n';
      } else if (event.type == juno::sdk::EventType::ToolCompleted) {
        std::cout << kYellow << "tool completed: " << event.tool_call.name << kReset << '\n';
      } else if (event.type == juno::sdk::EventType::CompactionStarted) {
        std::cout << kYellow << "Compacting conversation..." << kReset << '\n';
      } else if (event.type == juno::sdk::EventType::CompactionCompleted) {
        std::cout << kYellow << "Conversation compacted." << kReset << '\n';
      }
    });
    if (!result)
      std::cout << "\nRun failed: " << result.error().message << '\n';
    else {
      if (is_blank(result.value().final_text)) {
        if (!forecast_fallback.empty()) {
          std::cout << forecast_fallback;
        } else {
          std::cout << "Please provide a city so I can check the weather.";
        }
      }
      std::cout << '\n';
    }
  }
  curl_cleanup();
}
