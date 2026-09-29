/**
 * @file memory.hpp
 * @brief Memory management interfaces for the Juno SDK.
 *
 * This file defines the interfaces and classes related to memory management
 * within the Juno SDK. It includes definitions for memory entries, memory
 * stores, and the memory manager that orchestrates memory operations.
 *
 * The MemoryManager class exposes constrained Markdown file operations across
 * one or more durable memory stores.
 *
 * The MemoryStore interface allows for different implementations of memory storage,
 * enabling flexibility in how memories are persisted and retrieved.
 *
 * @note This file is part of the Juno SDK project and is intended for use
 *       within that context. It relies on the nlohmann::json library for JSON
 *       handling and the Expected class for error handling.
 *
 * @see MemoryStore, MemoryManager, MemoryManagerOptions
 */
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "juno_sdk/expected.hpp"

namespace juno::sdk {

struct MemoryStoreOptions {
  std::string name;
  std::string path;
  std::string description;
};

// MemoryStore is an abstract base class that defines the interface for memory storage.
class MemoryStore {
public:
  static std::shared_ptr<MemoryStore> create(MemoryStoreOptions options);
  static std::shared_ptr<MemoryStore> create(const std::string &name, const std::string &path);
  static std::shared_ptr<MemoryStore> create(const std::string &path);

  virtual ~MemoryStore() = default;
  virtual const std::string &name() const = 0;
  virtual const std::string &description() const = 0;
  /** Reads a Markdown file from this store's restricted directory. */
  virtual Expected<std::string> read_file(std::string_view path) const {
    return Error{ErrorCode::ToolExecutionFailed, "memory store does not support file operations"};
  }
  /** Creates or replaces a Markdown file in this store's restricted directory. */
  virtual Expected<void> write_file(std::string_view path, std::string_view content) {
    return Error{ErrorCode::ToolExecutionFailed, "memory store does not support file operations"};
  }
  /** Renames a Markdown file within this store's restricted directory. */
  virtual Expected<void> rename_file(std::string_view from, std::string_view to) {
    return Error{ErrorCode::ToolExecutionFailed, "memory store does not support file operations"};
  }
  /** Deletes a Markdown file from this store's restricted directory. */
  virtual Expected<void> delete_file(std::string_view path) {
    return Error{ErrorCode::ToolExecutionFailed, "memory store does not support file operations"};
  }
  /** Lists Markdown files in this store's restricted directory. */
  virtual Expected<std::vector<std::string>> list_files() const {
    return Error{ErrorCode::ToolExecutionFailed, "memory store does not support file operations"};
  }
};

struct MemoryManagerOptions {
  std::vector<std::shared_ptr<MemoryStore>> stores;
  bool file_tools_enabled{true};
};

// MemoryManager orchestrates memory operations across multiple memory stores.
class MemoryManager {
public:
  static std::shared_ptr<MemoryManager> create(MemoryManagerOptions options = {});

  explicit MemoryManager(MemoryManagerOptions options = {});

  MemoryManager &add_store(std::shared_ptr<MemoryStore> store);
  bool file_tools_enabled() const;
  const std::vector<std::shared_ptr<MemoryStore>> &stores() const;

  Expected<std::string> read_file(std::string_view path, const std::string &store = {}) const;
  Expected<void> write_file(std::string_view path, std::string_view content,
                            const std::string &store = {});
  Expected<void> rename_file(std::string_view from, std::string_view to,
                             const std::string &store = {});
  Expected<void> delete_file(std::string_view path, const std::string &store = {});

private:
  MemoryManagerOptions options_;
};

} // namespace juno::sdk
