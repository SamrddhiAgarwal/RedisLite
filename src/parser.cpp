#include "redislite/parser.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace redislite {
namespace {

std::string toUpper(std::string text) {
    std::ranges::transform(text, text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

void requireArgCount(std::size_t actual, std::size_t expected, const std::string& usage) {
    if (actual != expected) {
        throw ParseError("wrong number of arguments, usage: " + usage);
    }
}

std::chrono::seconds parseExpireSeconds(const std::string& text) {
    long long seconds = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [rest, error] = std::from_chars(begin, end, seconds);
    if (error != std::errc() || rest != end) {
        throw ParseError("EX value must be an integer number of seconds");
    }
    if (seconds <= 0) {
        throw ParseError("EX value must be greater than 0");
    }
    if (seconds > kMaxExpireSeconds) {
        throw ParseError("EX value is too large (maximum " + std::to_string(kMaxExpireSeconds) +
                         ")");
    }
    return std::chrono::seconds(seconds);
}

}  // namespace

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> tokens;
    std::string current;
    bool inToken = false;
    bool inQuotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (inQuotes) {
            const bool escape =
                c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\');
            if (escape) {
                current += line[++i];
            } else if (c == '"') {
                inQuotes = false;
                if (i + 1 < line.size() && !std::isspace(static_cast<unsigned char>(line[i + 1]))) {
                    throw ParseError("closing quote must be followed by a space");
                }
            } else {
                current += c;
            }
        } else if (c == '"') {
            inQuotes = true;
            inToken = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (inToken) {
                tokens.push_back(std::move(current));
                current.clear();
                inToken = false;
            }
        } else {
            current += c;
            inToken = true;
        }
    }

    if (inQuotes) {
        throw ParseError("unterminated quoted string");
    }
    if (inToken) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}

Command parseCommand(std::string_view line) {
    const std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) {
        throw ParseError("empty command");
    }

    const std::string name = toUpper(tokens[0]);
    const std::size_t argCount = tokens.size() - 1;
    Command command;

    if (name == "PING") {
        requireArgCount(argCount, 0, "PING");
        command.type = CommandType::Ping;
    } else if (name == "SET") {
        if (argCount != 2 && argCount != 4) {
            throw ParseError("wrong number of arguments, usage: SET key value [EX seconds]");
        }
        command.type = CommandType::Set;
        command.key = tokens[1];
        command.value = tokens[2];
        if (argCount == 4) {
            if (toUpper(tokens[3]) != "EX") {
                throw ParseError("unsupported SET option '" + tokens[3] + "', expected EX");
            }
            command.expireAfter = parseExpireSeconds(tokens[4]);
        }
    } else if (name == "GET" || name == "DEL" || name == "EXISTS" || name == "TTL") {
        requireArgCount(argCount, 1, name + " key");
        command.key = tokens[1];
        if (name == "GET") {
            command.type = CommandType::Get;
        } else if (name == "DEL") {
            command.type = CommandType::Del;
        } else if (name == "EXISTS") {
            command.type = CommandType::Exists;
        } else {
            command.type = CommandType::Ttl;
        }
    } else if (name == "SAVE") {
        requireArgCount(argCount, 0, "SAVE");
        command.type = CommandType::Save;
    } else if (name == "QUIT") {
        requireArgCount(argCount, 0, "QUIT");
        command.type = CommandType::Quit;
    } else {
        throw ParseError("unknown command '" + tokens[0] + "'");
    }
    return command;
}

}  // namespace redislite
