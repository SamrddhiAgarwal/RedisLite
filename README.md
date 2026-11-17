# RedisLite

A small, Redis-inspired in-memory key-value database written in C++20. It stores string keys and
values in a hash table, supports TTL expiration and LRU eviction, saves snapshots to disk, and
serves clients over TCP with one thread per client.

It is a learning project, not a Redis replacement. It does not speak the Redis (RESP) protocol.

## Features

- `std::unordered_map` key-value storage, thread-safe through a `std::mutex`
- Tokenizer and parser with quoted values (`SET greeting "hello world"`)
- Case-insensitive commands: `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `TTL`, `SAVE`, `QUIT`
- TTL expiration (`SET key value EX seconds`), checked lazily when a key is accessed
- LRU eviction when a configurable key limit is reached
- Snapshot persistence: `SAVE` writes a file, which is loaded again at startup
- Blocking POSIX TCP server (thread per client) and an interactive client
- GoogleTest suite run through CTest, plus a small timing tool (SET, GET, SAVE, LOAD)

## Technology

C++20, STL, POSIX sockets, `std::thread` / `std::mutex`, CMake, GoogleTest. Linux only.

## Architecture

```
client ──TCP──> Server ──(one thread per client)──> CommandExecutor ──> Database
                                                          │
                                                          └──> parser (tokenize + validate)
```

| Component | Files | Responsibility |
|-----------|-------|----------------|
| `Database` | `database.hpp/.cpp` | Storage, TTL, LRU, save/load. Owns the only mutex. |
| Parser | `parser.hpp/.cpp`, `command.hpp` | Splits text into tokens and builds a validated `Command`. |
| `CommandExecutor` | `executor.hpp/.cpp` | Runs one command line against the database and formats the reply. |
| `Socket` | `socket.hpp/.cpp` | RAII file descriptor, `sendAll`, buffered `readLine`. Shared by server and client. |
| `Server` | `server.hpp/.cpp` | Accept loop, worker threads, shutdown. |
| `Client` | `client.hpp/.cpp` | Connects, sends a line, reads one reply line; interactive prompt. |

Parsing, execution, and storage are kept separate: the parser knows nothing about the database,
and the database knows nothing about text commands or sockets.

## Build

Requires CMake 3.20+, a C++20 compiler (GCC 13 or newer), and GoogleTest
(`sudo apt install cmake g++ libgtest-dev` on Ubuntu).

```bash
cmake -S . -B build                          # Release by default
cmake --build build -j

cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j
```

## Tests

```bash
ctest --test-dir build --output-on-failure
```

The tests cover tokenizing and parsing, the executor responses, SET/GET/DEL/EXISTS, TTL (using
a fake clock, so nothing sleeps), LRU eviction and recency, save/load round trips, concurrent
database access, and TCP client-server smoke tests on a random free port.

## Run

```bash
./build/redislite server --port 6379 --max-keys 1000 --data-file dump.rdb
./build/redislite client --host 127.0.0.1 --port 6379
./build/redislite --help
```

All options are optional; the values above are the defaults. The server listens on
`127.0.0.1` only. Stop it with `Ctrl+C` (SIGINT) or SIGTERM.

## Commands

One command per line; one reply line per command. Command names are case-insensitive.
Use double quotes for values with spaces; inside quotes, `\"` and `\\` are escapes. A closing
quote must be followed by a space or the end of the line (`"a"b` is rejected).

| Command | Reply |
|---------|-------|
| `PING` | `PONG` |
| `SET key value` | `OK` |
| `SET key value EX seconds` | `OK` (seconds must be 1 to 315360000) |
| `GET key` | `VALUE <value>` or `NOT_FOUND` |
| `DEL key` | `1` if deleted, `0` if missing |
| `EXISTS key` | `1` or `0` |
| `TTL key` | seconds left, `-1` if no expiry, `-2` if missing or expired |
| `SAVE` | `OK` or `ERROR save failed: ...` |
| `QUIT` | `BYE`, then the server closes the connection |

Invalid input gets a reply starting with `ERROR`, for example
`ERROR wrong number of arguments, usage: GET key`.

```
redislite> SET greeting "hello world"
OK
redislite> GET greeting
VALUE hello world
redislite> SET session abc EX 60
OK
redislite> TTL session
60
redislite> DEL greeting
1
redislite> GET greeting
NOT_FOUND
```

## How TTL works

Each entry has an optional expiration time (`std::chrono::system_clock`). Expiration is
**lazy**: an expired key is removed only when something accesses it (`GET`, `EXISTS`, `DEL`,
`TTL`, `SET`) or when space is needed for a new key. `TTL` rounds up, so a live key never
reports `0`. A plain `SET` on an existing key removes its expiry.

## How LRU works

The database keeps a `std::list` of keys ordered from most to least recently used, and each
hash-table entry stores an iterator to its list node. `SET` and successful `GET` move the node
to the front with `list::splice`, which is O(1) and keeps every iterator valid. `DEL` removes
the node through the stored iterator, so the list never holds stale keys. `EXISTS` and `TTL` do
not change recency.

When a new key is inserted and the database is full, it first removes all expired keys. If it
is still full, it evicts the key at the back of the list (the least recently used).

## Persistence

`SAVE` writes a snapshot; on startup the server loads it if the file exists. The snapshot is
written to `<file>.tmp` and then renamed over the old file, so a failed save does not destroy
the previous snapshot. Keys are written from least to most recently used, so loading rebuilds
the same LRU order. Expired keys are skipped when saving and when loading.

File format (length-prefixed, so spaces, quotes, and newlines in keys or values are safe):

```
REDISLITE 1
<expires_at_ms> <key_length> <value_length>
<key bytes><value bytes>
```

`expires_at_ms` is milliseconds since the Unix epoch, or `-1` for no expiry. The second and
third lines repeat for each key. A file with a bad header or truncated record is rejected with
an error instead of being partly loaded.

## Timing utility

```bash
./build/redislite_benchmark          # 100000 operations
./build/redislite_benchmark 500000
```

On one thread it times N in-process `SET` calls, N `GET` calls, one `saveToFile` of the N keys
to a temporary file and one `loadFromFile` of that file. It prints elapsed time and approximate
operations (or keys) per second. It does not measure networking, and results depend on the
machine and build type (use a Release build).

## Known limitations

- Data is written to disk only when `SAVE` is called; changes since the last `SAVE` are lost on
  shutdown or crash.
- One global mutex serializes all database operations, so threads do not run commands in
  parallel.
- One thread per client does not scale to large numbers of connections.
- When the database is full and some keys have a TTL, inserting a new key scans every key for
  expired entries (O(n)). Without TTL keys, eviction is O(1).
- Expired keys that are never accessed stay in memory until eviction needs the space or they
  are skipped by `SAVE`.
- Not RESP-compatible; values cannot contain newlines when sent over the network.
- No authentication or encryption; the server binds to `127.0.0.1` only.
- Linux/POSIX only.
