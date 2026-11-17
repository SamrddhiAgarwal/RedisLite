#include "redislite/client.hpp"

#include <netdb.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace redislite {

void Client::connect(const std::string& host, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const int status = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &results);
    if (status != 0) {
        throw std::runtime_error("cannot resolve host '" + host + "': " + ::gai_strerror(status));
    }
    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(results, &::freeaddrinfo);

    std::string lastError = "no addresses found";
    for (const addrinfo* address = results; address != nullptr; address = address->ai_next) {
        Socket socket(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!socket.isValid()) {
            lastError = std::strerror(errno);
            continue;
        }
        if (::connect(socket.fd(), address->ai_addr, address->ai_addrlen) == 0) {
            socket_ = std::move(socket);
            return;
        }
        lastError = std::strerror(errno);
    }
    throw std::runtime_error("cannot connect to " + host + ":" + std::to_string(port) + ": " +
                             lastError);
}

std::string Client::sendCommand(const std::string& line) {
    if (!socket_.isValid()) {
        throw std::runtime_error("not connected");
    }
    if (line.find('\n') != std::string::npos) {
        throw std::invalid_argument("a command must be a single line");
    }
    if (!socket_.sendAll(line + "\n")) {
        throw std::runtime_error("failed to send command: connection lost");
    }
    std::string response;
    if (socket_.readLine(response) != ReadStatus::Line) {
        throw std::runtime_error("server closed the connection");
    }
    return response;
}

int runInteractiveClient(const std::string& host, std::uint16_t port) {
    Client client;
    try {
        client.connect(host, port);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Connected to " << host << ':' << port << ". Type QUIT to exit.\n";

    std::string line;
    while (true) {
        std::cout << "redislite> " << std::flush;
        if (!std::getline(std::cin, line)) {
            std::cout << '\n';
            return 0;
        }
        if (line.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        try {
            const std::string response = client.sendCommand(line);
            std::cout << response << '\n';
            if (response == "BYE") {
                return 0;
            }
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << '\n';
            return 1;
        }
    }
}

}  // namespace redislite
