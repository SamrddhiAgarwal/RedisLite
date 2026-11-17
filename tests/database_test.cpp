#include "redislite/database.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_helpers.hpp"

using redislite::Database;
using redislite::testing::TempFile;
using namespace std::chrono_literals;

namespace {

// A clock the test moves forward by hand, so TTL tests never sleep and never flake.
class FakeClock {
public:
    Database::NowFunction function() {
        return [this] { return now_; };
    }
    void advance(std::chrono::milliseconds amount) { now_ += amount; }

private:
    Database::TimePoint now_{std::chrono::seconds(1'700'000'000)};
};

}  // namespace

// ---------- Basic operations ----------

TEST(DatabaseTest, SetThenGetReturnsValue) {
    Database db(10);
    db.set("name", "redis");
    EXPECT_EQ(db.get("name"), "redis");
}

TEST(DatabaseTest, GetMissingKeyReturnsNothing) {
    Database db(10);
    EXPECT_FALSE(db.get("missing").has_value());
}

TEST(DatabaseTest, SetOverwritesExistingValue) {
    Database db(10);
    db.set("k", "first");
    db.set("k", "second");
    EXPECT_EQ(db.get("k"), "second");
    EXPECT_EQ(db.size(), 1u);
}

TEST(DatabaseTest, DelRemovesKeyOnce) {
    Database db(10);
    db.set("k", "v");
    EXPECT_TRUE(db.del("k"));
    EXPECT_FALSE(db.del("k"));
    EXPECT_FALSE(db.get("k").has_value());
}

TEST(DatabaseTest, ExistsReflectsPresence) {
    Database db(10);
    EXPECT_FALSE(db.exists("k"));
    db.set("k", "v");
    EXPECT_TRUE(db.exists("k"));
}

TEST(DatabaseTest, RejectsZeroCapacity) { EXPECT_THROW(Database(0), std::invalid_argument); }

// ---------- TTL ----------

TEST(DatabaseTtlTest, ReportsMinusOneWithoutExpiryAndMinusTwoWhenMissing) {
    Database db(10);
    db.set("k", "v");
    EXPECT_EQ(db.ttl("k"), Database::kNoExpiry);
    EXPECT_EQ(db.ttl("missing"), Database::kMissing);
}

TEST(DatabaseTtlTest, CountsDownAndRoundsUp) {
    FakeClock clock;
    Database db(10, clock.function());
    db.set("k", "v", 10s);
    EXPECT_EQ(db.ttl("k"), 10);
    clock.advance(2500ms);
    EXPECT_EQ(db.ttl("k"), 8);
    clock.advance(7499ms);
    EXPECT_EQ(db.ttl("k"), 1);
    clock.advance(1ms);
    EXPECT_EQ(db.ttl("k"), Database::kMissing);
}

TEST(DatabaseTtlTest, ExpiredKeyIsHiddenAndRemovedLazily) {
    FakeClock clock;
    Database db(10, clock.function());
    db.set("k", "v", 5s);
    clock.advance(5s);
    EXPECT_EQ(db.size(), 1u);  // still stored: nothing has touched it yet
    EXPECT_FALSE(db.get("k").has_value());
    EXPECT_EQ(db.size(), 0u);  // removed by the GET
}

TEST(DatabaseTtlTest, ExpiredKeyIsNotCountedByExistsOrDel) {
    FakeClock clock;
    Database db(10, clock.function());
    db.set("a", "v", 1s);
    db.set("b", "v", 1s);
    clock.advance(1s);
    EXPECT_FALSE(db.exists("a"));
    EXPECT_FALSE(db.del("b"));
    EXPECT_EQ(db.size(), 0u);
}

TEST(DatabaseTtlTest, SetWithoutExpiryClearsPreviousTtl) {
    FakeClock clock;
    Database db(10, clock.function());
    db.set("k", "v", 5s);
    db.set("k", "v2");
    clock.advance(10s);
    EXPECT_EQ(db.get("k"), "v2");
    EXPECT_EQ(db.ttl("k"), Database::kNoExpiry);
}

TEST(DatabaseTtlTest, RejectsNonPositiveExpiry) {
    Database db(10);
    EXPECT_THROW(db.set("k", "v", 0s), std::invalid_argument);
    EXPECT_THROW(db.set("k", "v", -1s), std::invalid_argument);
    EXPECT_FALSE(db.exists("k"));
}

// ---------- LRU eviction ----------

TEST(DatabaseLruTest, EvictsLeastRecentlyUsedAtCapacity) {
    Database db(2);
    db.set("a", "1");
    db.set("b", "2");
    db.set("c", "3");
    EXPECT_FALSE(db.exists("a"));
    EXPECT_TRUE(db.exists("b"));
    EXPECT_TRUE(db.exists("c"));
    EXPECT_EQ(db.size(), 2u);
}

TEST(DatabaseLruTest, GetMakesKeyMostRecentlyUsed) {
    Database db(2);
    db.set("a", "1");
    db.set("b", "2");
    db.get("a");
    db.set("c", "3");
    EXPECT_TRUE(db.exists("a"));
    EXPECT_FALSE(db.exists("b"));
}

TEST(DatabaseLruTest, UpdatingKeyRefreshesRecencyWithoutEvicting) {
    Database db(2);
    db.set("a", "1");
    db.set("b", "2");
    db.set("a", "updated");
    EXPECT_EQ(db.size(), 2u);
    db.set("c", "3");
    EXPECT_EQ(db.get("a"), "updated");
    EXPECT_FALSE(db.exists("b"));
}

TEST(DatabaseLruTest, DeletedKeyLeavesNoStaleEntry) {
    Database db(2);
    db.set("a", "1");
    db.set("b", "2");
    db.del("a");
    db.set("c", "3");  // fits without eviction
    EXPECT_TRUE(db.exists("b"));
    EXPECT_TRUE(db.exists("c"));
    db.set("d", "4");  // now b is the least recently used
    EXPECT_FALSE(db.exists("b"));
    EXPECT_TRUE(db.exists("c"));
    EXPECT_TRUE(db.exists("d"));
}

TEST(DatabaseLruTest, ExpiredKeysAreRemovedBeforeEvictingLiveKeys) {
    FakeClock clock;
    Database db(2, clock.function());
    db.set("live", "1");      // least recently used, no expiry
    db.set("temp", "2", 5s);  // more recent, but will expire
    clock.advance(6s);
    db.set("new", "3");
    EXPECT_TRUE(db.exists("live"));
    EXPECT_TRUE(db.exists("new"));
    EXPECT_EQ(db.size(), 2u);
}

// ---------- Persistence ----------

TEST(DatabasePersistenceTest, RoundTripPreservesValuesAndExpiry) {
    TempFile file("roundtrip.rdb");
    FakeClock clock;
    Database original(10, clock.function());
    original.set("spaces", "hello big world");
    original.set("quotes", R"(she said "hi" \ bye)");
    original.set("newline", "line1\nline2");
    original.set("empty", "");
    original.set("key with spaces", "v");
    original.set("session", "abc", 100s);
    original.saveToFile(file.path());

    Database restored(10, clock.function());
    EXPECT_EQ(restored.loadFromFile(file.path()), 6u);
    EXPECT_EQ(restored.get("spaces"), "hello big world");
    EXPECT_EQ(restored.get("quotes"), R"(she said "hi" \ bye)");
    EXPECT_EQ(restored.get("newline"), "line1\nline2");
    EXPECT_EQ(restored.get("empty"), "");
    EXPECT_EQ(restored.get("key with spaces"), "v");
    EXPECT_EQ(restored.ttl("session"), 100);
    EXPECT_EQ(restored.ttl("spaces"), Database::kNoExpiry);
}

TEST(DatabasePersistenceTest, ExpiredRecordsAreSkippedOnLoad) {
    TempFile file("expired.rdb");
    FakeClock clock;
    Database original(10, clock.function());
    original.set("short", "v", 10s);
    original.set("long", "v", 60s);
    original.set("forever", "v");
    original.saveToFile(file.path());

    clock.advance(30s);  // "restart" 30 seconds later
    Database restored(10, clock.function());
    EXPECT_EQ(restored.loadFromFile(file.path()), 2u);
    EXPECT_FALSE(restored.exists("short"));
    EXPECT_EQ(restored.ttl("long"), 30);
    EXPECT_TRUE(restored.exists("forever"));
}

TEST(DatabasePersistenceTest, LoadRestoresLruOrder) {
    TempFile file("lru.rdb");
    Database original(3);
    original.set("a", "1");
    original.set("b", "2");
    original.set("c", "3");
    original.get("a");  // order from least to most recent: b, c, a
    original.saveToFile(file.path());

    Database restored(3);
    restored.loadFromFile(file.path());
    restored.set("d", "4");
    EXPECT_FALSE(restored.exists("b"));
    EXPECT_TRUE(restored.exists("a"));
    EXPECT_TRUE(restored.exists("c"));
}

TEST(DatabasePersistenceTest, MissingFileMeansEmptyDatabase) {
    TempFile file("does_not_exist.rdb");
    Database db(10);
    EXPECT_EQ(db.loadFromFile(file.path()), 0u);
    EXPECT_EQ(db.size(), 0u);
}

TEST(DatabasePersistenceTest, CorruptFilesAreRejected) {
    TempFile file("corrupt.rdb");
    Database db(10);

    std::ofstream(file.path()) << "not a redislite file\n";
    EXPECT_THROW(db.loadFromFile(file.path()), std::runtime_error);

    std::ofstream(file.path()) << "REDISLITE 1\n-1 10 5\nabc";
    EXPECT_THROW(db.loadFromFile(file.path()), std::runtime_error);

    std::ofstream(file.path()) << "REDISLITE 1\n-1 3\n";
    EXPECT_THROW(db.loadFromFile(file.path()), std::runtime_error);

    std::ofstream(file.path()) << "REDISLITE 1\n-1 -3 1\nab\n";
    EXPECT_THROW(db.loadFromFile(file.path()), std::runtime_error);
}

// ---------- Concurrency ----------

TEST(DatabaseConcurrencyTest, ParallelWritersAndReadersSeeConsistentData) {
    constexpr int kThreads = 8;
    constexpr int kKeysPerThread = 1000;
    Database db(kThreads * kKeysPerThread + 1);
    std::atomic<int> mismatches{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&db, &mismatches, t] {
            for (int i = 0; i < kKeysPerThread; ++i) {
                const std::string key = "t" + std::to_string(t) + ":" + std::to_string(i);
                db.set(key, key);
                if (db.get(key) != key) {
                    ++mismatches;
                }
                db.set("shared", key);  // every thread also fights over one key
                db.exists("shared");
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(mismatches.load(), 0);
    EXPECT_EQ(db.size(), static_cast<std::size_t>(kThreads * kKeysPerThread + 1));
    EXPECT_EQ(db.get("t3:999"), "t3:999");
}

TEST(DatabaseConcurrencyTest, ParallelInsertsNeverExceedCapacity) {
    constexpr std::size_t kCapacity = 100;
    Database db(kCapacity);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&db, t] {
            for (int i = 0; i < 2000; ++i) {
                db.set("t" + std::to_string(t) + ":" + std::to_string(i), "v");
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    EXPECT_EQ(db.size(), kCapacity);
}
