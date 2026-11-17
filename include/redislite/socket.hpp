#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace redislite {

inline constexpr std::size_t kMaxLineLength = 1024 * 1024;

enum class ReadStatus { Line, Closed, Error };

// Owns a socket file descriptor and closes it in the destructor (RAII). Move-only.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd);
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    int fd() const { return fd_; }
    bool isValid() const { return fd_ >= 0; }

    // send() may write only part of the data, so this loops until everything is sent.
    bool sendAll(std::string_view data) const;

    // Returns the next '\n'-terminated line without the "\r\n". Bytes received after the
    // newline stay buffered for the next call, so several commands in one recv() all work.
    ReadStatus readLine(std::string& line);

    // Wakes any thread blocked in recv() or accept() on this socket.
    void shutdown() const;
    void close();

private:
    int fd_ = -1;
    std::string buffer_;
};

// "<operation> failed: <strerror(errno)>"
std::string systemErrorMessage(const std::string& operation);

}  // namespace redislite
