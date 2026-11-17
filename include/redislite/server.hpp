#pragma once

#include <atomic>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "redislite/database.hpp"
#include "redislite/socket.hpp"

namespace redislite {

// Blocking TCP server on 127.0.0.1 using one thread per connected client.
class Server {
public:
    // An empty dataFile disables SAVE.
    Server(Database& database, std::string dataFile);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // Binds and listens. Port 0 lets the OS choose a free port. Throws std::runtime_error.
    void start(std::uint16_t port);
    std::uint16_t port() const;

    // Accepts clients until stop() is called, then waits for every client thread to finish.
    void run();

    // Safe to call from any thread and more than once.
    void stop();

private:
    struct ClientConnection {
        Socket socket;
        std::thread worker;
        std::atomic<bool> finished{false};
    };

    void handleClient(ClientConnection& connection);
    void reapFinishedClients();
    void joinAllClients();

    Database& database_;
    std::string dataFile_;
    Socket listener_;
    std::uint16_t port_ = 0;
    std::atomic<bool> stopping_{false};

    std::mutex clientsMutex_;
    std::list<std::unique_ptr<ClientConnection>> clients_;
};

}  // namespace redislite
