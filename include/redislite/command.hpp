#pragma once

#include <chrono>
#include <optional>
#include <string>

namespace redislite {

enum class CommandType { Ping, Set, Get, Del, Exists, Ttl, Save, Quit };

// A validated command produced by the parser. Fields a command does not use stay empty.
struct Command {
    CommandType type = CommandType::Ping;
    std::string key;
    std::string value;
    std::optional<std::chrono::seconds> expireAfter;
};

}  // namespace redislite
