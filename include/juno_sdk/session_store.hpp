#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

#include "juno_sdk/expected.hpp"
#include "juno_sdk/types.hpp"

namespace juno::sdk {

struct SessionStoreOptions {
  std::filesystem::path directory;
};

struct SessionInfo {
  std::string id;
  std::string title;
  /** Timestamps in microseconds since the Unix epoch. */
  std::int64_t created_at;
  std::int64_t updated_at;
};

struct SavedSession {
  SessionInfo info;
  ConversationSnapshot snapshot;
};

/** Local, opt-in storage for conversation checkpoints. */
class SessionStore {
public:
  [[nodiscard]] static Expected<SessionStore> create(SessionStoreOptions options);

  /** Lists readable sessions from most to least recently updated. */
  [[nodiscard]] Expected<std::vector<SessionInfo>> list() const;
  [[nodiscard]] Expected<SavedSession> load(std::string_view id) const;
  /** Atomically creates a checkpoint or replaces the specified existing checkpoint. */
  [[nodiscard]] Expected<SessionInfo> save(const ConversationSnapshot &snapshot,
                                           std::optional<std::string_view> id = std::nullopt) const;
  [[nodiscard]] Expected<void> erase(std::string_view id) const;

private:
  explicit SessionStore(std::filesystem::path directory);
  std::filesystem::path directory_;
};

} // namespace juno::sdk
