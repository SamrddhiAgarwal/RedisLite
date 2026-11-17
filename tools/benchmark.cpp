#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

#include "redislite/database.hpp"

namespace {

using Clock = std::chrono::steady_clock;

void report(const char* name, std::size_t count, const char* unit, Clock::duration elapsed) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double perSecond = seconds > 0 ? static_cast<double>(count) / seconds : 0.0;
    std::cout << std::fixed << std::setprecision(2) << name << ": " << count << ' ' << unit
              << " in " << seconds * 1000.0 << " ms (~" << std::setprecision(0) << perSecond << ' '
              << unit << "/sec)\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    std::size_t count = 100'000;
    if (argc > 2) {
        std::cerr << "Usage: redislite_benchmark [operation_count]\n";
        return 2;
    }
    if (argc == 2) {
        const std::string text = argv[1];
        const char* end = text.data() + text.size();
        const auto [rest, error] = std::from_chars(text.data(), end, count);
        if (error != std::errc() || rest != end || count == 0) {
            std::cerr << "operation_count must be a positive integer\n"
                      << "Usage: redislite_benchmark [operation_count]\n";
            return 2;
        }
    }

    // Keys are built before timing so only database calls are measured.
    std::vector<std::string> keys;
    std::vector<std::string> values;
    keys.reserve(count);
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys.push_back("key:" + std::to_string(i));
        values.push_back("value:" + std::to_string(i));
    }

    // Capacity equals the operation count so no eviction happens during the run.
    redislite::Database database(count);

    auto start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        database.set(keys[i], values[i]);
    }
    const auto setElapsed = Clock::now() - start;

    std::size_t hits = 0;
    start = Clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        if (database.get(keys[i])) {
            ++hits;
        }
    }
    const auto getElapsed = Clock::now() - start;

    const std::filesystem::path snapshot =
        std::filesystem::temp_directory_path() /
        ("redislite_benchmark_" + std::to_string(::getpid()) + ".rdb");
    Clock::duration saveElapsed{};
    Clock::duration loadElapsed{};
    std::size_t loaded = 0;
    try {
        start = Clock::now();
        database.saveToFile(snapshot.string());
        saveElapsed = Clock::now() - start;

        redislite::Database restored(count);
        start = Clock::now();
        loaded = restored.loadFromFile(snapshot.string());
        loadElapsed = Clock::now() - start;
    } catch (const std::exception& error) {
        std::cerr << "persistence timing failed: " << error.what() << '\n';
        std::error_code ignored;
        std::filesystem::remove(snapshot, ignored);
        return 1;
    }
    std::error_code ignored;
    std::filesystem::remove(snapshot, ignored);

    report("SET ", count, "ops", setElapsed);
    report("GET ", count, "ops", getElapsed);
    report("SAVE", count, "keys", saveElapsed);
    report("LOAD", loaded, "keys", loadElapsed);
    if (hits != count || loaded != count) {
        std::cerr << "unexpected result: " << hits << " GET hits and " << loaded
                  << " keys loaded, expected " << count << '\n';
        return 1;
    }
    std::cout << "Single thread, direct in-process calls; SAVE/LOAD use a temporary file. "
                 "No networking is measured. Numbers vary by machine and build type.\n";
    return 0;
}
