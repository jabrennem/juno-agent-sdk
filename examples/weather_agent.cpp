#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <curl/curl.h>

#include "juno_sdk/juno_sdk.hpp"

namespace {

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

  // Create a tool for converting temperatures between Celsius and Fahrenheit
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

  // Load steering files
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

  // Optionally enable saved sessions in ~/.juno/sessions/weather
  std::optional<juno::sdk::SessionStore> sessions;
  if (const char *home = std::getenv("HOME"); home && *home) {
    auto store = juno::sdk::SessionStore::create({
        .directory = std::filesystem::path(home) / ".juno/sessions/weather",
    });
    if (store)
      sessions = std::move(store.value());
    else
      std::cerr << "Could not enable saved weather sessions: " << store.error().message << '\n';
  }

  // Create a harness to run the interactive Weather Agent example
  auto harness = juno::sdk::TerminalHarness::create({
      .agent = std::move(weather_agent),
      .show_tools = true,
      .show_reasoning = true,
      .prompt = "[weather] > ",
      .banner = "Weather Agent (" + std::to_string(kContextSize) + " context tokens, " +
                std::to_string(kGenerationTokens) + " generation tokens)",
      .help = "Ask for a weather forecast for any city.\n"
              "Example: What's the weather in Seattle for five days?\n"
              "Automatic compaction: enabled at 75% of the model context budget.",
      .compaction = juno::sdk::CompactionOptions{
          .preserve_recent_turns = 1,
          .focus = "Preserve user preferences and recent weather context.",
      },
      .empty_response = "Please provide a city so I can check the weather.",
      .session_store = std::move(sessions),
  });

  // Run the interactive Weather Agent example
  auto result = harness.run_interactive();
  curl_cleanup();
  if (!result) {
    std::cerr << result.error().message << '\n';
    return 1;
  }
}
