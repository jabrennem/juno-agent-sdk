#include "juno_sdk/session_store.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <random>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

namespace juno::sdk {
namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr int kVersion = 1;
constexpr std::string_view kHex = "0123456789abcdef";
constexpr std::array<std::string_view, 4> kRoles{"system", "user", "assistant", "tool"};

Error storage_error(std::string message) {
  return {ErrorCode::SessionStorageFailed, std::move(message)};
}

bool valid_id(std::string_view id) {
  return id.size() == 32 && id.find_first_not_of(kHex) == std::string_view::npos;
}

std::string random_id() {
  std::random_device random;
  std::uniform_int_distribution<std::size_t> digit(0, kHex.size() - 1);
  std::string id(32, '0');
  for (char &character : id)
    character = kHex[digit(random)];
  return id;
}

fs::path session_path(const fs::path &directory, std::string_view id) {
  return directory / (std::string(id) + ".json");
}

std::int64_t timestamp() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string session_title(const ConversationSnapshot &snapshot) {
  const auto user = std::find_if(snapshot.history.begin(), snapshot.history.end(),
                                 [](const Message &message) { return message.role == Role::User; });
  if (user == snapshot.history.end())
    return "Untitled session";
  const auto start = user->content.find_first_not_of(" \t\n\r");
  if (start == std::string::npos)
    return "Untitled session";
  const std::string_view text = std::string_view(user->content).substr(start);
  const auto line_end = std::min(text.size(), text.find_first_of("\r\n"));
  auto end = std::min(line_end, std::size_t{80});
  while (end < line_end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
    --end;
  return std::string(text.substr(0, end));
}

Json encode_messages(const std::vector<Message> &messages) {
  auto encoded = Json::array();
  for (const auto &message : messages) {
    auto calls = Json::array();
    for (const auto &call : message.tool_calls) {
      calls.push_back(
          {{"id", call.id}, {"name", call.name}, {"arguments_json", call.arguments_json}});
    }
    encoded.push_back({
        {"role", kRoles.at(static_cast<std::size_t>(message.role))},
        {"content", message.content},
        {"tool_calls", std::move(calls)},
        {"tool_call_id", message.tool_call_id},
        {"tool_name", message.tool_name},
    });
  }
  return encoded;
}

std::vector<Message> decode_messages(const Json &encoded) {
  std::vector<Message> messages;
  const auto &entries = encoded.get_ref<const Json::array_t &>();
  messages.reserve(entries.size());
  for (const auto &entry : entries) {
    const auto role_name = entry.at("role").get<std::string>();
    const auto role = std::find(kRoles.begin(), kRoles.end(), role_name);
    if (role == kRoles.end())
      throw std::runtime_error("invalid message role");
    Message message{
        static_cast<Role>(role - kRoles.begin()),    entry.at("content").get<std::string>(),   {},
        entry.at("tool_call_id").get<std::string>(), entry.at("tool_name").get<std::string>(),
    };
    for (const auto &call : entry.at("tool_calls").get_ref<const Json::array_t &>()) {
      message.tool_calls.push_back({
          call.at("id").get<std::string>(),
          call.at("name").get<std::string>(),
          call.at("arguments_json").get<std::string>(),
      });
    }
    messages.push_back(std::move(message));
  }
  return messages;
}

/** A uniquely reserved staging directory on the same filesystem as the destination. */
class StagingDirectory {
public:
  explicit StagingDirectory(const fs::path &parent) {
    do {
      path_ = parent / (".saving-" + random_id());
    } while (!fs::create_directory(path_));
  }

  ~StagingDirectory() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }

  [[nodiscard]] fs::path file() const { return path_ / "checkpoint"; }

private:
  fs::path path_;
};

} // namespace

SessionStore::SessionStore(fs::path directory) : directory_(std::move(directory)) {}

Expected<SessionStore> SessionStore::create(SessionStoreOptions options) {
  if (options.directory.empty())
    return Error{ErrorCode::InvalidConfiguration, "session storage directory is required"};
  try {
    auto directory = fs::absolute(options.directory);
    fs::create_directories(directory);
    return SessionStore(std::move(directory));
  } catch (const fs::filesystem_error &error) {
    return storage_error(error.what());
  }
}

Expected<std::vector<SessionInfo>> SessionStore::list() const {
  try {
    std::vector<SessionInfo> sessions;
    for (const auto &entry : fs::directory_iterator(directory_)) {
      if (entry.path().extension() != ".json")
        continue;
      const auto id = entry.path().stem().string();
      if (!valid_id(id))
        continue;
      auto session = load(id);
      if (session)
        sessions.push_back(std::move(session.value().info));
    }
    std::sort(sessions.begin(), sessions.end(), [](const SessionInfo &a, const SessionInfo &b) {
      if (a.updated_at != b.updated_at)
        return a.updated_at > b.updated_at;
      return a.id < b.id;
    });
    return sessions;
  } catch (const fs::filesystem_error &error) {
    return storage_error(error.what());
  }
}

Expected<SavedSession> SessionStore::load(std::string_view id) const {
  if (!valid_id(id))
    return storage_error("invalid session ID");
  try {
    const auto path = session_path(directory_, id);
    if (!fs::is_regular_file(fs::symlink_status(path)))
      return storage_error("session file is missing or is not a regular file");
    std::ifstream input(path, std::ios::binary);
    if (!input)
      return storage_error("could not open session file");
    const auto document = Json::parse(input);
    if (document.at("version") != kVersion)
      return storage_error("unsupported session file version");
    if (document.at("id") != id)
      return storage_error("session ID does not match its filename");
    return SavedSession{
        SessionInfo{
            std::string(id),
            document.at("title").get<std::string>(),
            document.at("created_at").get<std::int64_t>(),
            document.at("updated_at").get<std::int64_t>(),
        },
        ConversationSnapshot{
            decode_messages(document.at("history")),
            decode_messages(document.at("context")),
        },
    };
  } catch (const std::exception &error) {
    return storage_error("could not read session " + std::string(id) + ": " + error.what());
  }
}

Expected<SessionInfo> SessionStore::save(const ConversationSnapshot &snapshot,
                                         std::optional<std::string_view> id) const {
  try {
    const auto now = timestamp();
    SessionInfo info{{}, session_title(snapshot), now, now};
    if (id) {
      auto previous = load(*id);
      if (!previous)
        return previous.error();
      info = std::move(previous.value().info);
      info.updated_at = std::max(info.updated_at, now);
    } else {
      do {
        info.id = random_id();
      } while (fs::exists(session_path(directory_, info.id)));
    }
    const auto json =
        Json{
            {"version", kVersion},
            {"id", info.id},
            {"title", info.title},
            {"created_at", info.created_at},
            {"updated_at", info.updated_at},
            {"history", encode_messages(snapshot.history)},
            {"context", encode_messages(snapshot.context)},
        }
            .dump(2);
    const StagingDirectory staging(directory_);
    const auto temporary = staging.file();
    std::ofstream output(temporary, std::ios::binary);
    output << json << '\n';
    output.close();
    if (!output)
      return storage_error("could not write session checkpoint");
    fs::rename(temporary, session_path(directory_, info.id));
    return info;
  } catch (const std::exception &error) {
    return storage_error("could not save session: " + std::string(error.what()));
  }
}

Expected<void> SessionStore::erase(std::string_view id) const {
  if (!valid_id(id))
    return storage_error("invalid session ID");
  try {
    const auto path = session_path(directory_, id);
    if (fs::is_directory(fs::symlink_status(path)) || !fs::remove(path))
      return storage_error("session file could not be removed");
    return {};
  } catch (const fs::filesystem_error &error) {
    return storage_error(error.what());
  }
}

} // namespace juno::sdk
