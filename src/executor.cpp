#include "redislite/executor.hpp"

#include <exception>
#include <utility>

#include "redislite/parser.hpp"

namespace redislite {

CommandExecutor::CommandExecutor(Database& database, std::string dataFile)
    : database_(database), dataFile_(std::move(dataFile)) {}

Response CommandExecutor::execute(std::string_view line) {
    Command command;
    try {
        command = parseCommand(line);
    } catch (const ParseError& error) {
        return {std::string("ERROR ") + error.what()};
    }

    using enum CommandType;
    switch (command.type) {
        case Ping:
            return {"PONG"};
        case Set:
            database_.set(command.key, command.value, command.expireAfter);
            return {"OK"};
        case Get: {
            const std::optional<std::string> value = database_.get(command.key);
            return {value ? "VALUE " + *value : "NOT_FOUND"};
        }
        case Del:
            return {database_.del(command.key) ? "1" : "0"};
        case Exists:
            return {database_.exists(command.key) ? "1" : "0"};
        case Ttl:
            return {std::to_string(database_.ttl(command.key))};
        case Save:
            if (dataFile_.empty()) {
                return {"ERROR persistence is disabled (no data file configured)"};
            }
            try {
                database_.saveToFile(dataFile_);
            } catch (const std::exception& error) {
                return {std::string("ERROR save failed: ") + error.what()};
            }
            return {"OK"};
        case Quit:
            return {"BYE", true};
    }
    return {"ERROR unsupported command"};
}

}  // namespace redislite
