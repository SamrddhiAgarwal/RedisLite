#include "redislite/database.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace redislite {
namespace {

// File format is documented in README.md under "Persistence".
constexpr const char* kFileHeader = "REDISLITE 1";
constexpr long long kNoExpiryMarker = -1;
constexpr long long kMaxEpochMillis =
    std::chrono::duration_cast<std::chrono::milliseconds>(Database::TimePoint::duration::max())
        .count();

struct Record {
    std::string key;
    std::string value;
    long long expiresAtMs;
};

// C++20 guarantees system_clock counts from the Unix epoch, so saved timestamps stay valid.
long long toEpochMillis(Database::TimePoint time) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}

Database::TimePoint fromEpochMillis(long long millis) {
    return Database::TimePoint(std::chrono::milliseconds(millis));
}

std::runtime_error corruptFile(const std::string& path, const std::string& reason) {
    return std::runtime_error("data file '" + path + "' is corrupt: " + reason);
}

}  // namespace

Database::Database(std::size_t maxKeys, NowFunction now) : maxKeys_(maxKeys), now_(std::move(now)) {
    if (maxKeys_ == 0) {
        throw std::invalid_argument("maxKeys must be at least 1");
    }
}

void Database::set(const std::string& key, const std::string& value,
                   std::optional<std::chrono::seconds> expireAfter) {
    if (expireAfter && expireAfter->count() <= 0) {
        throw std::invalid_argument("expiration must be a positive number of seconds");
    }
    std::lock_guard lock(mutex_);
    const TimePoint now = now_();
    std::optional<TimePoint> expiresAt;
    if (expireAfter) {
        expiresAt = now + *expireAfter;
    }
    insertOrUpdate(key, value, expiresAt, now);
}

std::optional<std::string> Database::get(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto it = findLive(key, now_());
    if (it == entries_.end()) {
        return std::nullopt;
    }
    touch(it->second);
    return it->second.value;
}

bool Database::del(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto it = findLive(key, now_());
    if (it == entries_.end()) {
        return false;
    }
    erase(it);
    return true;
}

bool Database::exists(const std::string& key) {
    std::lock_guard lock(mutex_);
    return findLive(key, now_()) != entries_.end();
}

long long Database::ttl(const std::string& key) {
    std::lock_guard lock(mutex_);
    const TimePoint now = now_();
    auto it = findLive(key, now);
    if (it == entries_.end()) {
        return kMissing;
    }
    if (!it->second.expiresAt) {
        return kNoExpiry;
    }
    // Rounding up means a live key never reports 0 seconds left.
    return std::chrono::ceil<std::chrono::seconds>(*it->second.expiresAt - now).count();
}

std::size_t Database::size() const {
    std::lock_guard lock(mutex_);
    return entries_.size();
}

std::size_t Database::maxKeys() const { return maxKeys_; }

void Database::saveToFile(const std::string& path) const {
    std::lock_guard saveLock(saveMutex_);

    // Copy under the data lock, then write without it so other clients are not blocked by disk I/O.
    std::vector<Record> records;
    {
        std::lock_guard lock(mutex_);
        const TimePoint now = now_();
        records.reserve(entries_.size());
        // Least recently used first, so loading the file rebuilds the same LRU order.
        for (auto it = lru_.rbegin(); it != lru_.rend(); ++it) {
            const Entry& entry = entries_.at(*it);
            if (isExpired(entry, now)) {
                continue;
            }
            records.push_back(Record{
                .key = *it,
                .value = entry.value,
                .expiresAtMs = entry.expiresAt ? toEpochMillis(*entry.expiresAt) : kNoExpiryMarker,
            });
        }
    }

    // Write a temporary file and rename it, so a failed save never destroys the previous file.
    const std::string tempPath = path + ".tmp";
    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open '" + tempPath + "' for writing");
        }
        out << kFileHeader << '\n';
        for (const Record& record : records) {
            out << record.expiresAtMs << ' ' << record.key.size() << ' ' << record.value.size()
                << '\n'
                << record.key << record.value << '\n';
        }
        out.flush();
        if (!out) {
            throw std::runtime_error("failed while writing '" + tempPath + "'");
        }
    }

    std::error_code error;
    std::filesystem::rename(tempPath, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(tempPath, ignored);
        throw std::runtime_error("cannot replace '" + path + "': " + error.message());
    }
}

std::size_t Database::loadFromFile(const std::string& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        return 0;
    }
    const std::uintmax_t fileSize = std::filesystem::file_size(path, error);
    if (error) {
        throw std::runtime_error("cannot read data file '" + path + "': " + error.message());
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open data file '" + path + "'");
    }

    std::string header;
    if (!std::getline(in, header) || header != kFileHeader) {
        throw corruptFile(path, "missing RedisLite header");
    }

    std::vector<Record> records;
    long long expiresAtMs = 0;
    while (in >> expiresAtMs) {
        long long keyLength = 0;
        long long valueLength = 0;
        if (!(in >> keyLength >> valueLength)) {
            throw corruptFile(path, "incomplete record header");
        }
        const bool validHeader =
            expiresAtMs >= kNoExpiryMarker && expiresAtMs <= kMaxEpochMillis && keyLength >= 0 &&
            valueLength >= 0 &&
            static_cast<std::uintmax_t>(keyLength) + static_cast<std::uintmax_t>(valueLength) <=
                fileSize;
        if (!validHeader || in.get() != '\n') {
            throw corruptFile(path, "invalid record header");
        }

        Record record{
            .key = std::string(static_cast<std::size_t>(keyLength), '\0'),
            .value = std::string(static_cast<std::size_t>(valueLength), '\0'),
            .expiresAtMs = expiresAtMs,
        };
        in.read(record.key.data(), keyLength);
        in.read(record.value.data(), valueLength);
        if (!in || in.get() != '\n') {
            throw corruptFile(path, "truncated record");
        }
        records.push_back(std::move(record));
    }
    if (!in.eof()) {
        throw corruptFile(path, "invalid record header");
    }

    std::lock_guard lock(mutex_);
    entries_.clear();
    lru_.clear();
    keysWithExpiry_ = 0;

    const TimePoint now = now_();
    for (const Record& record : records) {
        std::optional<TimePoint> expiresAt;
        if (record.expiresAtMs != kNoExpiryMarker) {
            expiresAt = fromEpochMillis(record.expiresAtMs);
            if (*expiresAt <= now) {
                continue;
            }
        }
        insertOrUpdate(record.key, record.value, expiresAt, now);
    }
    return entries_.size();
}

Database::EntryMap::iterator Database::findLive(const std::string& key, TimePoint now) {
    auto it = entries_.find(key);
    if (it != entries_.end() && isExpired(it->second, now)) {
        erase(it);
        return entries_.end();
    }
    return it;
}

void Database::insertOrUpdate(const std::string& key, const std::string& value,
                              std::optional<TimePoint> expiresAt, TimePoint now) {
    auto it = entries_.find(key);
    if (it != entries_.end()) {
        Entry& entry = it->second;
        if (entry.expiresAt) {
            --keysWithExpiry_;
        }
        if (expiresAt) {
            ++keysWithExpiry_;
        }
        entry.value = value;
        entry.expiresAt = expiresAt;
        touch(entry);
        return;
    }

    if (entries_.size() >= maxKeys_) {
        makeRoomForNewKey(now);
    }
    lru_.push_front(key);
    try {
        entries_.emplace(key, Entry{
                                  .value = value,
                                  .expiresAt = expiresAt,
                                  .lruPosition = lru_.begin(),
                              });
    } catch (...) {
        lru_.pop_front();  // keep the list and the map in sync if allocation fails
        throw;
    }
    if (expiresAt) {
        ++keysWithExpiry_;
    }
}

Database::EntryMap::iterator Database::erase(EntryMap::iterator it) {
    if (it->second.expiresAt) {
        --keysWithExpiry_;
    }
    lru_.erase(it->second.lruPosition);
    return entries_.erase(it);
}

void Database::makeRoomForNewKey(TimePoint now) {
    // Expired keys are dropped first so a live key is only evicted when really necessary.
    if (keysWithExpiry_ > 0) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            it = isExpired(it->second, now) ? erase(it) : std::next(it);
        }
    }
    if (entries_.size() >= maxKeys_) {
        erase(entries_.find(lru_.back()));
    }
}

void Database::touch(Entry& entry) {
    // splice relinks the node in O(1) and keeps every iterator (including lruPosition) valid.
    lru_.splice(lru_.begin(), lru_, entry.lruPosition);
}

bool Database::isExpired(const Entry& entry, TimePoint now) {
    return entry.expiresAt && *entry.expiresAt <= now;
}

}  // namespace redislite
