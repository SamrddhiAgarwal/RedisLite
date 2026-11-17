#include "redislite/executor.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "test_helpers.hpp"

using redislite::CommandExecutor;
using redislite::Database;

namespace {

class CommandExecutorTest : public ::testing::Test {
protected:
    std::string run(const std::string& line) { return executor_.execute(line).text; }

    Database database_{100};
    CommandExecutor executor_{database_, ""};
};

}  // namespace

TEST_F(CommandExecutorTest, PingReturnsPong) { EXPECT_EQ(run("PING"), "PONG"); }

TEST_F(CommandExecutorTest, SetThenGet) {
    EXPECT_EQ(run(R"(SET greeting "hello world")"), "OK");
    EXPECT_EQ(run("GET greeting"), "VALUE hello world");
    EXPECT_EQ(run("GET missing"), "NOT_FOUND");
}

TEST_F(CommandExecutorTest, DelAndExistsReturnOneOrZero) {
    run("SET a 1");
    EXPECT_EQ(run("EXISTS a"), "1");
    EXPECT_EQ(run("DEL a"), "1");
    EXPECT_EQ(run("DEL a"), "0");
    EXPECT_EQ(run("EXISTS a"), "0");
}

TEST_F(CommandExecutorTest, TtlResponses) {
    EXPECT_EQ(run("TTL missing"), "-2");
    run("SET plain v");
    EXPECT_EQ(run("TTL plain"), "-1");
    run("SET temp v EX 100");
    EXPECT_EQ(run("TTL temp"), "100");
}

TEST_F(CommandExecutorTest, ErrorsStartWithErrorPrefix) {
    for (const char* line : {"FOO", "GET", "SET k v EX 0", R"(SET k "open)", ""}) {
        EXPECT_TRUE(run(line).starts_with("ERROR ")) << "line: " << line;
    }
}

TEST_F(CommandExecutorTest, OnlyQuitClosesTheConnection) {
    const auto quit = executor_.execute("quit");
    EXPECT_EQ(quit.text, "BYE");
    EXPECT_TRUE(quit.closeConnection);
    EXPECT_FALSE(executor_.execute("PING").closeConnection);
    EXPECT_FALSE(executor_.execute("BAD").closeConnection);
}

TEST_F(CommandExecutorTest, SaveWithoutDataFileIsAnError) {
    EXPECT_TRUE(run("SAVE").starts_with("ERROR "));
}

TEST(CommandExecutorSaveTest, SaveWritesLoadableFile) {
    redislite::testing::TempFile file("executor_save.rdb");
    Database database(10);
    CommandExecutor executor(database, file.path());
    executor.execute(R"(SET name "Ada Lovelace")");

    EXPECT_EQ(executor.execute("SAVE").text, "OK");

    Database restored(10);
    EXPECT_EQ(restored.loadFromFile(file.path()), 1u);
    EXPECT_EQ(restored.get("name"), "Ada Lovelace");
}

TEST(CommandExecutorSaveTest, SaveToUnwritablePathReportsError) {
    Database database(10);
    CommandExecutor executor(database, "/nonexistent-redislite-dir/dump.rdb");
    EXPECT_TRUE(executor.execute("SAVE").text.starts_with("ERROR save failed"));
}
