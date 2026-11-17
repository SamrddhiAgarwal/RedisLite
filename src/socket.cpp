#include "redislite/socket.hpp"

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace redislite {

Socket::Socket(int fd) : fd_(fd) {}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), buffer_(std::move(other.buffer_)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        buffer_ = std::move(other.buffer_);
    }
    return *this;
}

bool Socket::sendAll(std::string_view data) const {
    std::size_t sent = 0;
    while (sent < data.size()) {
        // MSG_NOSIGNAL: a disconnected peer gives EPIPE instead of killing the process with
        // SIGPIPE.
        const ssize_t written = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

ReadStatus Socket::readLine(std::string& line) {
    while (true) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            line.assign(buffer_, 0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return ReadStatus::Line;
        }
        if (buffer_.size() > kMaxLineLength) {
            return ReadStatus::Error;
        }

        char chunk[4096];
        const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (received == 0) {
            return ReadStatus::Closed;
        }
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ReadStatus::Error;
        }
        buffer_.append(chunk, static_cast<std::size_t>(received));
    }
}

void Socket::shutdown() const {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
    }
}

void Socket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    buffer_.clear();
}

std::string systemErrorMessage(const std::string& operation) {
    return operation + " failed: " + std::strerror(errno);
}

}  // namespace redislite
