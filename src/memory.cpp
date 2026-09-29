#include "juno_sdk/memory.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace juno::sdk {
namespace {

class MarkdownMemoryStore final : public MemoryStore {
public:
  explicit MarkdownMemoryStore(MemoryStoreOptions options)
      : name_(std::move(options.name)), description_(std::move(options.description)),
        directory_(expand_user(options.path)) {}

  const std::string &name() const override { return name_; }
  const std::string &description() const override { return description_; }

  Expected<std::string> read_file(std::string_view relative) const override {
    auto path = safe_path(relative);
    if (!path)
      return path.error();
    std::ifstream input(path.value(), std::ios::binary);
    if (!input)
      return Error{ErrorCode::ToolExecutionFailed,
                   "could not read memory file: " + std::string(relative)};
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
  }

  Expected<void> write_file(std::string_view relative, std::string_view content) override {
    auto path = safe_path(relative);
    if (!path)
      return path.error();
    try {
      std::filesystem::create_directories(directory_);
      std::ofstream output(path.value(), std::ios::binary | std::ios::trunc);
      if (!output)
        return Error{ErrorCode::ToolExecutionFailed,
                     "could not write memory file: " + path.value().string()};
      output << content;
    } catch (const std::exception &error) {
      return Error{ErrorCode::ToolExecutionFailed,
                   std::string{"could not write memory file: "} + error.what()};
    }
    return {};
  }

  Expected<void> rename_file(std::string_view from, std::string_view to) override {
    auto source = safe_path(from);
    auto destination = safe_path(to);
    if (!source)
      return source.error();
    if (!destination)
      return destination.error();
    try {
      std::filesystem::rename(source.value(), destination.value());
    } catch (const std::exception &error) {
      return Error{ErrorCode::ToolExecutionFailed,
                   std::string{"could not rename memory file: "} + error.what()};
    }
    return {};
  }

  Expected<void> delete_file(std::string_view relative) override {
    auto path = safe_path(relative);
    if (!path)
      return path.error();
    try {
      if (!std::filesystem::remove(path.value()))
        return Error{ErrorCode::ToolExecutionFailed,
                     "memory file does not exist: " + std::string(relative)};
    } catch (const std::exception &error) {
      return Error{ErrorCode::ToolExecutionFailed,
                   std::string{"could not delete memory file: "} + error.what()};
    }
    return {};
  }

  Expected<std::vector<std::string>> list_files() const override {
    std::vector<std::string> result;
    try {
      if (!std::filesystem::exists(directory_))
        return result;
      for (const auto &entry : std::filesystem::directory_iterator(directory_))
        if (entry.is_regular_file() && entry.path().extension() == ".md")
          result.push_back(entry.path().filename().string());
      std::sort(result.begin(), result.end());
    } catch (const std::exception &error) {
      return Error{ErrorCode::ToolExecutionFailed,
                   std::string{"could not list memory files: "} + error.what()};
    }
    return result;
  }

private:
  std::string name_;
  std::string description_;
  std::filesystem::path directory_;

  static std::filesystem::path expand_user(const std::string &path) {
    if (path.rfind("~/", 0) != 0)
      return path;
    const char *home = std::getenv("HOME");
    return home ? std::filesystem::path(home) / path.substr(2) : std::filesystem::path(path);
  }

  Expected<std::filesystem::path> safe_path(std::string_view relative) const {
    const std::filesystem::path requested(relative);
    if (relative.empty() || requested.is_absolute() || requested.filename() != requested
        || requested.extension() != ".md" || requested.filename() == ".")
      return Error{ErrorCode::ToolExecutionFailed,
                   "memory file path must be a single .md filename"};
    return directory_ / requested;
  }
};

template <typename Operation>
auto one_store(const MemoryManagerOptions &options, const std::string &name, Operation operation)
    -> decltype(operation(std::declval<MemoryStore &>())) {
  for (const auto &store : options.stores)
    if (name.empty() || store->name() == name)
      return operation(*store);
  using Result = decltype(operation(std::declval<MemoryStore &>()));
  return Result{Error{ErrorCode::ToolExecutionFailed, "memory store not found: " + name}};
}

} // namespace

MemoryManager::MemoryManager(MemoryManagerOptions options) : options_(std::move(options)) {}

MemoryManager &MemoryManager::add_store(std::shared_ptr<MemoryStore> store) {
  options_.stores.push_back(std::move(store));
  return *this;
}

bool MemoryManager::file_tools_enabled() const {
  return options_.file_tools_enabled;
}

const std::vector<std::shared_ptr<MemoryStore>> &MemoryManager::stores() const {
  return options_.stores;
}

Expected<std::string> MemoryManager::read_file(std::string_view path,
                                                const std::string &store) const {
  return one_store(options_, store, [path](MemoryStore &memory) { return memory.read_file(path); });
}

Expected<void> MemoryManager::write_file(std::string_view path, std::string_view content,
                                         const std::string &store) {
  return one_store(options_, store, [path, content](MemoryStore &memory) {
    return memory.write_file(path, content);
  });
}

Expected<void> MemoryManager::rename_file(std::string_view from, std::string_view to,
                                          const std::string &store) {
  return one_store(options_, store, [from, to](MemoryStore &memory) {
    return memory.rename_file(from, to);
  });
}

Expected<void> MemoryManager::delete_file(std::string_view path, const std::string &store) {
  return one_store(options_, store, [path](MemoryStore &memory) { return memory.delete_file(path); });
}

std::shared_ptr<MemoryManager> MemoryManager::create(MemoryManagerOptions options) {
  return std::make_shared<MemoryManager>(std::move(options));
}

std::shared_ptr<MemoryStore> MemoryStore::create(const std::string &name,
                                                 const std::string &path) {
  return create(MemoryStoreOptions{.name = name, .path = path});
}

std::shared_ptr<MemoryStore> MemoryStore::create(MemoryStoreOptions options) {
  if (options.description.empty())
    options.description = "Markdown files in a restricted memory directory.";
  return std::make_shared<MarkdownMemoryStore>(std::move(options));
}

std::shared_ptr<MemoryStore> MemoryStore::create(const std::string &path) {
  const auto filename = std::filesystem::path(path).filename().string();
  const auto name = std::filesystem::path(filename).stem().string();
  return create(name.empty() ? "memory" : name, path);
}

} // namespace juno::sdk
