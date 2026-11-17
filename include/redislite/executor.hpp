#pragma once

#include <string>
#include <string_view>

#include "redislite/database.hpp"

namespace redislite {

struct Response {
    std::string text;
    bool closeConnection = false;
};

// Turns one line of command text into a response: parse, validate, call the database.
class CommandExecutor {
public:
    // An empty dataFile disables SAVE.
    CommandExecutor(Database& database, std::string dataFile);

    Response execute(std::string_view line);

private:
    Database& database_;
    std::string dataFile_;
};

}  // namespace redislite
