#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "redislite/client.hpp"
#include "redislite/database.hpp"
#include "redislite/server.hpp"

namespace {

struct Options {
    std::string mode;
    std::uint16_t port = 6379;
    std::size_t maxKeys = 1000;
    std::string dataFile = "dump.rdb";
    std::string host = "127.0.0.1";
};

void printUsage(std::ostream& out) {
    out << "Usage:\n"
           "  redislite server [--port N] [--max-keys N] [--data-file PATH]\n"
           "  redislite client [--host HOST] [--port N]\n"
           "  redislite --help\n"
           "\n"
           "Defaults: --port 6379, --max-keys 1000, --data-file dump.rdb, --host 127.0.0.1\n";
}

unsigned long long parseUnsigned(const std::string& text, const std::string& flag,
                                 unsigned long long min, unsigned long long max) {
    unsigned long long value = 0;
    const char* end = text.data() + text.size();
    const auto [rest, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc() || rest != end || value < min || value > max) {
        throw std::invalid_argument(flag + " must be a number between " + std::to_string(min) +
                                    " and " + std::to_string(max));
    }
    return value;
}

const std::string& requireNonEmpty(const std::string& value, const std::string& flag) {
    if (value.empty()) {
        throw std::invalid_argument(flag + " must not be empty");
    }
    return value;
}

Options parseOptions(const std::vector<std::string>& args) {
    if (args.empty()) {
        throw std::invalid_argument("missing mode: expected 'server' or 'client'");
    }
    Options options;
    options.mode = args[0];
    const bool isServer = options.mode == "server";
    if (!isServer && options.mode != "client") {
        throw std::invalid_argument("unknown mode '" + options.mode + "'");
    }

    for (std::size_t i = 1; i < args.size(); i += 2) {
        const std::string& flag = args[i];
        if (i + 1 >= args.size()) {
            throw std::invalid_argument("missing value for " + flag);
        }
        const std::string& value = args[i + 1];
        if (flag == "--port") {
            options.port = static_cast<std::uint16_t>(parseUnsigned(value, flag, 1, 65535));
        } else if (flag == "--max-keys" && isServer) {
            options.maxKeys = static_cast<std::size_t>(parseUnsigned(value, flag, 1, 100'000'000));
        } else if (flag == "--data-file" && isServer) {
            options.dataFile = requireNonEmpty(value, flag);
        } else if (flag == "--host" && !isServer) {
            options.host = requireNonEmpty(value, flag);
        } else {
            throw std::invalid_argument("invalid option '" + flag + "' for " + options.mode +
                                        " mode");
        }
    }
    return options;
}

int runServer(const Options& options) {
    try {
        redislite::Database database(options.maxKeys);
        const std::size_t loaded = database.loadFromFile(options.dataFile);
        std::cout << "Loaded " << loaded << " key(s) from " << options.dataFile << '\n';

        redislite::Server server(database, options.dataFile);
        server.start(options.port);

        // Block SIGINT/SIGTERM in every thread; one dedicated thread waits for them and stops
        // the server. This avoids doing unsafe work inside an asynchronous signal handler.
        sigset_t signals;
        sigemptyset(&signals);
        sigaddset(&signals, SIGINT);
        sigaddset(&signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &signals, nullptr);
        std::thread signalThread([&server, signals] {
            int received = 0;
            sigwait(&signals, &received);
            server.stop();
        });

        std::cout << "RedisLite listening on 127.0.0.1:" << server.port()
                  << " (max keys: " << options.maxKeys << "). Press Ctrl+C to stop.\n";
        int exitCode = 0;
        try {
            server.run();
        } catch (const std::exception& error) {
            std::cerr << "error: " << error.what() << '\n';
            exitCode = 1;
        }

        // If run() ended without a signal, wake the waiting thread so it can be joined.
        ::kill(::getpid(), SIGTERM);
        signalThread.join();
        std::cout << "Server stopped.\n";
        return exitCode;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    for (const std::string& arg : args) {
        if (arg == "--help" || arg == "-h") {
            printUsage(std::cout);
            return 0;
        }
    }

    Options options;
    try {
        options = parseOptions(args);
    } catch (const std::invalid_argument& error) {
        std::cerr << "error: " << error.what() << "\n\n";
        printUsage(std::cerr);
        return 2;
    }

    if (options.mode == "client") {
        return redislite::runInteractiveClient(options.host, options.port);
    }
    return runServer(options);
}
