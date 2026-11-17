#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "redislite/command.hpp"

namespace redislite {

// Largest accepted EX value (10 years), which keeps expiry time arithmetic far from overflow.
inline constexpr long long kMaxExpireSeconds = 315'360'000;

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Splits on whitespace. Double quotes group words; inside quotes, \" and \\ are escapes.
// Throws ParseError on an unterminated quote or text directly after a closing quote.
std::vector<std::string> tokenize(std::string_view line);

// Throws ParseError for empty input, unknown commands, wrong argument counts or bad EX values.
Command parseCommand(std::string_view line);

}  // namespace redislite
