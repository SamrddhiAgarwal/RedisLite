#include "redislite/server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "redislite/executor.hpp"

namespace redislite {

Server::Server(Database& database, std::string dataFile)
    : database_(database), dataFile_(std::move(dataFile)) {}

Server::~Server() {
    stop();
    joinAllClients();
}

void Server::start(std::uint16_t port) {
    Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
    if (!listener.isValid()) {
        throw std::runtime_error(systemErrorMessage("socket"));
    }

    // Allows restarting the server immediately without waiting for old connections to time out.
    const int reuse = 1;
    ::setsockopt(listener.fd(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(listener.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error(systemErrorMessage("bind to port " + std::to_string(port)));
    }
    if (::listen(listener.fd(), SOMAXCONN) < 0) {
        throw std::runtime_error(systemErrorMessage("listen"));
    }

    socklen_t length = sizeof(address);
    if (::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address), &length) < 0) {
        throw std::runtime_error(systemErrorMessage("getsockname"));
    }
    port_ = ntohs(address.sin_port);
    listener_ = std::move(listener);
}

std::uint16_t Server::port() const { return port_; }

void Server::run() {
    while (!stopping_) {
        const int clientFd = ::accept(listener_.fd(), nullptr, nullptr);
        if (clientFd < 0) {
            const int error = errno;
            if (stopping_) {
                break;
            }
            if (error == EBADF || error == EINVAL || error == ENOTSOCK) {
                std::cerr << "accept failed: " << std::strerror(error) << '\n';
                break;  // the listening socket itself is unusable
            }
            if (error == EMFILE || error == ENFILE || error == ENOBUFS || error == ENOMEM) {
                // Out of descriptors or memory: close finished connections, wait, then retry.
                std::cerr << "accept failed: " << std::strerror(error) << '\n';
                reapFinishedClients();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // EINTR, ECONNABORTED and network errors of the pending connection: just retry.
            continue;
        }

        reapFinishedClients();

        auto connection = std::make_unique<ClientConnection>();
        connection->socket = Socket(clientFd);

        std::lock_guard lock(clientsMutex_);
        // Checked under the lock so stop() can never miss a connection added after it ran.
        if (stopping_) {
            break;
        }
        try {
            connection->worker = std::thread(&Server::handleClient, this, std::ref(*connection));
        } catch (const std::system_error& error) {
            // Thread limit reached: drop this client (its socket closes) but keep serving others.
            std::cerr << "cannot start client thread: " << error.what() << '\n';
            continue;
        }
        clients_.push_back(std::move(connection));
    }

    stop();
    joinAllClients();
}

void Server::stop() {
    stopping_ = true;
    // On Linux, shutdown() on the listening socket makes a blocked accept() return.
    listener_.shutdown();

    std::lock_guard lock(clientsMutex_);
    for (const auto& connection : clients_) {
        connection->socket.shutdown();
    }
}

void Server::handleClient(ClientConnection& connection) {
    try {
        CommandExecutor executor(database_, dataFile_);
        std::string line;
        while (connection.socket.readLine(line) == ReadStatus::Line) {
            const Response response = executor.execute(line);
            if (!connection.socket.sendAll(response.text + "\n") || response.closeConnection) {
                break;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "client error: " << error.what() << '\n';
    }
    // Lets the client see the disconnect now; the descriptor is closed once this thread is joined.
    connection.socket.shutdown();
    connection.finished = true;
}

void Server::reapFinishedClients() {
    std::list<std::unique_ptr<ClientConnection>> finished;
    {
        std::lock_guard lock(clientsMutex_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            auto current = it++;
            if ((*current)->finished) {
                finished.splice(finished.end(), clients_, current);
            }
        }
    }
    for (const auto& connection : finished) {
        connection->worker.join();
    }
}

void Server::joinAllClients() {
    std::list<std::unique_ptr<ClientConnection>> remaining;
    {
        std::lock_guard lock(clientsMutex_);
        remaining.swap(clients_);
    }
    for (const auto& connection : remaining) {
        if (connection->worker.joinable()) {
            connection->worker.join();
        }
    }
}

}  // namespace redislite
