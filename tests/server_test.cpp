#include "redislite/server.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "redislite/client.hpp"

using redislite::Client;
using redislite::Database;
using redislite::ReadStatus;
using redislite::Server;
using redislite::Socket;

namespace {

class ServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_.start(0);  // OS picks a free port, so tests never clash with a running server
        serverThread_ = std::thread([this] { server_.run(); });
    }

    void TearDown() override {
        server_.stop();
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
    }

    Client connectClient() {
        Client client;
        client.connect("127.0.0.1", server_.port());
        return client;
    }

    // A raw socket lets a test control exactly how bytes are split across send() calls.
    Socket connectRaw() {
        Socket socket(::socket(AF_INET, SOCK_STREAM, 0));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(server_.port());
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(socket.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("raw connect failed");
        }
        return socket;
    }

    static std::string readLine(Socket& socket) {
        std::string line;
        if (socket.readLine(line) != ReadStatus::Line) {
            throw std::runtime_error("connection closed before a full line arrived");
        }
        return line;
    }

    Database database_{1000};
    Server server_{database_, ""};
    std::thread serverThread_;
};

}  // namespace

TEST_F(ServerTest, ClientCommandsRoundTrip) {
    Client client = connectClient();
    EXPECT_EQ(client.sendCommand("PING"), "PONG");
    EXPECT_EQ(client.sendCommand(R"(SET greeting "hello world")"), "OK");
    EXPECT_EQ(client.sendCommand("GET greeting"), "VALUE hello world");
    EXPECT_EQ(client.sendCommand("TTL greeting"), "-1");
    EXPECT_TRUE(client.sendCommand("NOPE").starts_with("ERROR "));
    EXPECT_EQ(client.sendCommand("QUIT"), "BYE");
    EXPECT_THROW(client.sendCommand("PING"), std::runtime_error);
}

TEST_F(ServerTest, ClientsShareOneDatabase) {
    Client writer = connectClient();
    Client reader = connectClient();
    EXPECT_EQ(writer.sendCommand("SET shared 42"), "OK");
    EXPECT_EQ(reader.sendCommand("GET shared"), "VALUE 42");
    EXPECT_EQ(database_.get("shared"), "42");
}

TEST_F(ServerTest, HandlesSeveralCommandsInOneSendAndSplitCommands) {
    Socket socket = connectRaw();
    ASSERT_TRUE(socket.sendAll("PING\nSET a 1\nGET a\n"));
    EXPECT_EQ(readLine(socket), "PONG");
    EXPECT_EQ(readLine(socket), "OK");
    EXPECT_EQ(readLine(socket), "VALUE 1");

    ASSERT_TRUE(socket.sendAll("GE"));
    ASSERT_TRUE(socket.sendAll("T a\r\n"));
    EXPECT_EQ(readLine(socket), "VALUE 1");
}

TEST_F(ServerTest, LargeValueSurvivesPartialReadsAndWrites) {
    // Far bigger than one recv() chunk (4 KiB), so both sides must loop over partial transfers.
    const std::string big(500'000, 'x');
    Client client = connectClient();
    EXPECT_EQ(client.sendCommand("SET big " + big), "OK");
    EXPECT_EQ(client.sendCommand("GET big"), "VALUE " + big);
}

TEST_F(ServerTest, ServerKeepsWorkingAfterAbruptDisconnect) {
    {
        Socket socket = connectRaw();
        ASSERT_TRUE(socket.sendAll("SET half"));  // no newline, then the socket closes
    }
    Client client = connectClient();
    EXPECT_EQ(client.sendCommand("PING"), "PONG");
    EXPECT_EQ(client.sendCommand("EXISTS half"), "0");
}

TEST_F(ServerTest, ConcurrentClientsAllSucceed) {
    constexpr int kClients = 4;
    constexpr int kCommandsPerClient = 50;
    std::vector<std::thread> threads;
    std::vector<int> failures(kClients, 0);
    for (int c = 0; c < kClients; ++c) {
        threads.emplace_back([this, c, &failures] {
            Client client = connectClient();
            for (int i = 0; i < kCommandsPerClient; ++i) {
                const std::string key = "c" + std::to_string(c) + ":" + std::to_string(i);
                if (client.sendCommand("SET " + key + " v") != "OK") {
                    ++failures[c];
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    for (int c = 0; c < kClients; ++c) {
        EXPECT_EQ(failures[c], 0) << "client " << c;
    }
    EXPECT_EQ(database_.size(), static_cast<std::size_t>(kClients * kCommandsPerClient));
}

TEST_F(ServerTest, StopDisconnectsConnectedClients) {
    Client client = connectClient();
    EXPECT_EQ(client.sendCommand("PING"), "PONG");

    server_.stop();
    serverThread_.join();  // returns only after every client thread has finished

    EXPECT_THROW(client.sendCommand("PING"), std::runtime_error);
}
