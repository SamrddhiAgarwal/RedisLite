#include "redislite/parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using redislite::CommandType;
using redislite::parseCommand;
using redislite::ParseError;
using redislite::tokenize;
using Tokens = std::vector<std::string>;

TEST(TokenizerTest, SplitsOnAnyWhitespace) {
    EXPECT_EQ(tokenize("  SET\tkey   value "), (Tokens{"SET", "key", "value"}));
}

TEST(TokenizerTest, EmptyOrBlankLineHasNoTokens) {
    EXPECT_TRUE(tokenize("").empty());
    EXPECT_TRUE(tokenize("   \t ").empty());
}

TEST(TokenizerTest, QuotedValueKeepsSpaces) {
    EXPECT_EQ(tokenize(R"(SET greeting "hello world")"),
              (Tokens{"SET", "greeting", "hello world"}));
}

TEST(TokenizerTest, EscapesInsideQuotes) {
    EXPECT_EQ(tokenize(R"(SET k "say \"hi\" \\ ok")"), (Tokens{"SET", "k", R"(say "hi" \ ok)"}));
}

TEST(TokenizerTest, EmptyQuotedStringIsAToken) {
    EXPECT_EQ(tokenize(R"(SET k "")"), (Tokens{"SET", "k", ""}));
}

TEST(TokenizerTest, TextDirectlyAfterClosingQuoteThrows) {
    EXPECT_THROW(tokenize(R"(SET k "hello"world)"), ParseError);
    EXPECT_THROW(tokenize(R"(SET k ""x)"), ParseError);
    EXPECT_EQ(tokenize("SET k \"a b\"\t"), (Tokens{"SET", "k", "a b"}));
}

TEST(TokenizerTest, UnterminatedQuoteThrows) {
    EXPECT_THROW(tokenize(R"(SET k "hello)"), ParseError);
}

TEST(ParserTest, ParsesPing) { EXPECT_EQ(parseCommand("PING").type, CommandType::Ping); }

TEST(ParserTest, CommandNamesAreCaseInsensitive) {
    EXPECT_EQ(parseCommand("ping").type, CommandType::Ping);
    EXPECT_EQ(parseCommand("Get key").type, CommandType::Get);
    EXPECT_EQ(parseCommand("sEt key value").type, CommandType::Set);
}

TEST(ParserTest, ParsesSetWithoutExpiry) {
    const auto command = parseCommand(R"(SET greeting "hello world")");
    EXPECT_EQ(command.type, CommandType::Set);
    EXPECT_EQ(command.key, "greeting");
    EXPECT_EQ(command.value, "hello world");
    EXPECT_FALSE(command.expireAfter.has_value());
}

TEST(ParserTest, ParsesSetWithExpiry) {
    const auto command = parseCommand("SET session abc ex 30");
    ASSERT_TRUE(command.expireAfter.has_value());
    EXPECT_EQ(command.expireAfter->count(), 30);
    EXPECT_EQ(command.value, "abc");
}

TEST(ParserTest, RejectsInvalidExpiry) {
    EXPECT_THROW(parseCommand("SET k v EX 0"), ParseError);
    EXPECT_THROW(parseCommand("SET k v EX -5"), ParseError);
    EXPECT_THROW(parseCommand("SET k v EX abc"), ParseError);
    EXPECT_THROW(parseCommand("SET k v EX 10s"), ParseError);
    EXPECT_THROW(parseCommand(R"(SET k v EX "")"), ParseError);
    EXPECT_THROW(parseCommand("SET k v EX 99999999999999999999"), ParseError);
    EXPECT_THROW(parseCommand("SET k v EX 315360001"), ParseError);
}

TEST(ParserTest, RejectsUnknownSetOption) {
    EXPECT_THROW(parseCommand("SET k v PX 10"), ParseError);
}

TEST(ParserTest, RejectsWrongSetArgumentCount) {
    EXPECT_THROW(parseCommand("SET"), ParseError);
    EXPECT_THROW(parseCommand("SET k"), ParseError);
    EXPECT_THROW(parseCommand("SET k v EX"), ParseError);
    EXPECT_THROW(parseCommand("SET k v EX 5 extra"), ParseError);
}

TEST(ParserTest, KeyCommandsNeedExactlyOneKey) {
    const std::pair<const char*, CommandType> commands[] = {{"GET", CommandType::Get},
                                                            {"DEL", CommandType::Del},
                                                            {"EXISTS", CommandType::Exists},
                                                            {"TTL", CommandType::Ttl}};
    for (const auto& [name, type] : commands) {
        const std::string text = name;
        const auto command = parseCommand(text + " mykey");
        EXPECT_EQ(command.type, type) << text;
        EXPECT_EQ(command.key, "mykey") << text;
        EXPECT_THROW(parseCommand(text), ParseError) << text;
        EXPECT_THROW(parseCommand(text + " a b"), ParseError) << text;
    }
}

TEST(ParserTest, NoArgumentCommandsRejectExtraArguments) {
    EXPECT_EQ(parseCommand("SAVE").type, CommandType::Save);
    EXPECT_EQ(parseCommand("QUIT").type, CommandType::Quit);
    EXPECT_THROW(parseCommand("PING hello"), ParseError);
    EXPECT_THROW(parseCommand("SAVE now"), ParseError);
    EXPECT_THROW(parseCommand("QUIT please"), ParseError);
}

TEST(ParserTest, RejectsUnknownAndEmptyCommands) {
    EXPECT_THROW(parseCommand("FLUSHALL"), ParseError);
    EXPECT_THROW(parseCommand(""), ParseError);
    EXPECT_THROW(parseCommand("   "), ParseError);
}
