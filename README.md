# Limit Order Book & Matching Engine Server (C++17)

An in-memory limit order book with **price-time priority** matching, plus a small
single-threaded TCP server that accepts text orders and returns fills.
Correctness first; performance work comes after measurement.

## Architecture

```
 client(s)                      server (one thread, one poll() loop)
 ┌────────┐   "ADD 7 BUY 101 10\n"   ┌──────────────┐   ┌───────────┐
 │ client │ ───────────────────────► │ line framing │──►│ protocol  │──► OrderBook
 │        │ ◄─────────────────────── │ + sockets    │◄──│ parse/fmt │◄── (matching)
 └────────┘   "TRADE 7 3 100 4\nOK\n"└──────────────┘   └───────────┘
```

| Layer | Files | Knows about |
|---|---|---|
| Matching engine | `order_book.{hpp,cpp}` | prices, orders, trades. No text, no sockets |
| Protocol | `protocol.{hpp,cpp}` | text <-> commands, replies. No sockets |
| Server / client | `server.cpp`, `client.cpp` | sockets and line framing. No matching logic |

Keeping the protocol free of sockets means the whole text path is unit-testable
without opening a port.

### Why the server is single-threaded

All connections feed one loop that applies commands in the order their bytes are
read, so **arrival order at the server defines time priority**. With several
threads you would need locking (or a queue) to preserve that order anyway, and
the lock would sit on the hot path. Real matching engines are commonly built
around a single sequencer for this reason. Scaling out means sharding by
instrument, not threading one book.

## Project layout

```
include/orderbook/   order_book.hpp  protocol.hpp  workload.hpp
src/                 order_book.cpp  protocol.cpp  server.cpp  client.cpp  main.cpp
tests/               test_order_book.cpp  test_protocol.cpp
bench/               bench_book.cpp
CMakeLists.txt
```

## Build and test

Requires a C++17 compiler and CMake 3.14+.

**Linux / macOS / WSL**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Add `-DENABLE_SANITIZERS=ON` for an AddressSanitizer + UBSan build (GCC/Clang).

**Windows (Visual Studio / MSVC)**

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\demo.exe
```

The engine, protocol tests, benchmark and demo build natively on Windows. The
**server and client use POSIX sockets**, so CMake skips them on Windows. Run them
under WSL (see below).

**No CMake?**

```bash
g++ -std=c++17 -O2 -Iinclude src/order_book.cpp tests/test_order_book.cpp -o order_book_tests && ./order_book_tests
g++ -std=c++17 -O2 -Iinclude src/order_book.cpp src/protocol.cpp tests/test_protocol.cpp -o protocol_tests && ./protocol_tests
```

## Run the server and client

Open two terminals (Linux, macOS, or WSL) in the project folder.

```bash
# terminal 1
./build/server 12000

# terminal 2: pick one
nc 127.0.0.1 12000                                        # type commands by hand
./build/client --port 12000 --file orders.txt             # replay a file of commands
./build/client --port 12000 --random 100000 --quiet       # random flow + latency summary
```

`client` options: `--host`, `--port`, `--file F`, `--random N`, `--seed S`,
`--quiet` (suppress replies, keep the summary). With no `--file` or `--random` it
reads commands from stdin. Ctrl-C stops the server and prints a summary.

**WSL on Windows.** In an Ubuntu terminal, your Windows drive is under `/mnt/c/`:

```bash
cd "/mnt/c/path/to/orderbook"
sudo apt install -y g++ cmake            # first time only
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
./build-linux/server 12000
```

Use a separate build folder (`build-linux`) so it does not clash with the Windows `build` folder.

## Protocol

One command per line, terminated by `\n` (`\r\n` accepted), case-insensitive.

| Request | Meaning |
|---|---|
| `ADD <id> <BUY\|SELL> <price> <qty>` | Limit order; matches, then rests any remainder |
| `CANCEL <id>` | Cancel a resting order |

Every request is answered with zero or more `TRADE` lines and exactly one
terminating `OK` or `ERR <reason>`:

```
TRADE <taker_id> <maker_id> <price> <qty>     (price = the resting order's price)
OK
ERR <reason>
```

Error reasons: `empty_command`, `unknown_command`, `wrong_arg_count`, `bad_id`,
`bad_side`, `bad_price` (must be a positive integer), `bad_qty` (must be a
positive integer), `not_found` (cancel), `duplicate order id` (add),
`line_too_long`. Malformed input never reaches the book.

Example session:

```
> ADD 1 SELL 101 10
OK
> ADD 2 SELL 102 5
OK
> ADD 3 BUY 102 12
TRADE 3 1 101 10
TRADE 3 2 102 2
OK
> CANCEL 2
OK
> CANCEL 2
ERR not_found
```

The server listens on **127.0.0.1 only**: the protocol has no authentication.

## Design

| Piece | Choice | Why |
|---|---|---|
| Prices | `int64` ticks | No floating-point equality bugs |
| Each side | `std::map<Price, std::list<Order>>` | Sorted levels, FIFO within a level (time priority) |
| Cancel | `unordered_map<OrderId, {level it, order it}>` | Locate and unlink an order without scanning |
| Trade price | The resting (maker) order's price | Incoming orders get price improvement |
| Server I/O | non-blocking sockets + `poll()` | Many clients, one thread, no locks |

**Complexity** (L = non-empty price levels): cancel is O(1) amortised; adding a
non-crossing order is O(log L); a crossing order is O(log L) plus O(1) per fill.

## Testing

- `order_book_tests`: unit tests (no cross, full and partial fills, multi-level
  sweeps, price and time priority, cancels, invalid input) plus a randomized
  test: 60,000 random add/cancel operations run against a deliberately naive
  reference book; trades and book state must match throughout.
- `protocol_tests`: valid and malformed lines (empty, unknown command, wrong
  argument counts, negative, zero, overflowing and non-numeric fields, bad
  side), exact reply formatting, bad input leaves the book untouched, a
  format/parse round trip, and 20,000 random commands through the text path
  compared against calling the book directly.

## Benchmark

`bench_book` and `client` use the same workload generator (`workload.hpp`:
35% cancels, 65% limit orders, 50/50 sides, prices 95..105, qty 1..20), so the
numbers are comparable.

```bash
./build/bench_book 1000000                               # book alone
./build/server 12000 &
./build/client --port 12000 --random 1000000 --quiet     # full path over loopback
```

`bench_book` reports throughput from an untimed pass, and p50/p99/p99.9 from a
second pass that times each operation (the timer adds some overhead, so treat
those latencies as upper bounds). The client waits for each reply, so its figure
is round-trip latency over loopback, not server-side service time. The gap
between the two is the cost of parsing plus the network stack.

Results (1,000,000 ops, Release build, seed 12345; fill in your hardware):

| Setup | Compiler / OS | Throughput | p50 | p99 |
|---|---|---|---|---|
| Book only | MSVC 19.51, Windows | 2.56M ops/s | ~300 ns | ~1.2 µs |
| Book only | _GCC, WSL_ | _measure_ | _measure_ | _measure_ |
| Server, 1 client, loopback | _GCC, WSL_ | _measure_ | _measure_ (RTT) | _measure_ (RTT) |

CPU: _fill in_. Windows timer resolution is coarse (latencies come out in
multiples of 100 ns), so read the Windows p50 as approximate. Compare book-only
and server numbers only when they come from the same machine and compiler.

## Roadmap

- [x] Text protocol, single-threaded TCP server, test client, protocol tests
- [x] Benchmark harness (book only and full server)
- [ ] Fill in the remaining benchmark rows; profile, then optimise (memory pool
      for orders, flat array indexed by price tick). Record before and after.
- [ ] Market orders, immediate-or-cancel, modify/replace.
- [ ] Market-maker bot that quotes against the book (PnL vs inventory risk).

## Known limitations

- Heap allocation per order (`std::list`); no custom allocator yet.
- One instrument, limit orders only.
- Server and client are POSIX-only (Linux, macOS, WSL); loopback-only, no
  authentication, no market-data feed (clients only see replies to their own requests).
- Fills are reported to the taker only; the maker is not notified.
