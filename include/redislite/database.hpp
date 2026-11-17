#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace redislite {

// Thread-safe in-memory key-value store with lazy TTL expiration and LRU eviction.
// Every public method locks the internal mutex, so callers never need their own locking.
class Database {
public:
    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;
    using NowFunction = std::function<TimePoint()>;

    static constexpr long long kNoExpiry = -1;
    static constexpr long long kMissing = -2;

    // `now` lets tests control time without sleeping; normal code uses the system clock.
    explicit Database(std::size_t maxKeys, NowFunction now = Clock::now);

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Inserts or replaces a key. Setting without expireAfter removes any previous expiry.
    void set(const std::string& key, const std::string& value,
             std::optional<std::chrono::seconds> expireAfter = std::nullopt);
    std::optional<std::string> get(const std::string& key);
    bool del(const std::string& key);
    bool exists(const std::string& key);
    // Remaining seconds (rounded up), kNoExpiry, or kMissing.
    long long ttl(const std::string& key);

    // Number of stored entries; may include expired keys that have not been accessed yet.
    std::size_t size() const;
    std::size_t maxKeys() const;

    // Throws std::runtime_error on I/O failure.
    void saveToFile(const std::string& path) const;
    // Replaces the current contents. A missing file is treated as an empty database.
    // Returns the number of keys loaded. Throws std::runtime_error on a corrupt file.
    std::size_t loadFromFile(const std::string& path);

private:
    using LruList = std::list<std::string>;

    struct Entry {
        std::string value;
        std::optional<TimePoint> expiresAt;
        LruList::iterator lruPosition;
    };
    using EntryMap = std::unordered_map<std::string, Entry>;

    // The helpers below require mutex_ to be held by the caller.
    EntryMap::iterator findLive(const std::string& key, TimePoint now);
    void insertOrUpdate(const std::string& key, const std::string& value,
                        std::optional<TimePoint> expiresAt, TimePoint now);
    EntryMap::iterator erase(EntryMap::iterator it);
    void makeRoomForNewKey(TimePoint now);
    void touch(Entry& entry);
    static bool isExpired(const Entry& entry, TimePoint now);

    const std::size_t maxKeys_;
    const NowFunction now_;

    mutable std::mutex mutex_;
    EntryMap entries_;
    LruList lru_;  // front = most recently used, back = least recently used
    std::size_t keysWithExpiry_ = 0;

    // Serializes SAVE calls so two clients never write the same temporary file at once.
    mutable std::mutex saveMutex_;
};

}  // namespace redislite
