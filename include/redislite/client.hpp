#pragma once

#include <cstdint>
#include <string>

#include "redislite/socket.hpp"

namespace redislite {

class Client {
public:
    // Throws std::runtime_error if the host cannot be resolved or the connection fails.
    void connect(const std::string& host, std::uint16_t port);

    // Sends one command line and waits for its one-line response.
    // Throws std::runtime_error if the server has disconnected.
    std::string sendCommand(const std::string& line);

private:
    Socket socket_;
};

// Interactive prompt on stdin/stdout. Returns the process exit code.
int runInteractiveClient(const std::string& host, std::uint16_t port);

}  // namespace redislite
