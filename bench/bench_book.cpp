// Measures the OrderBook alone (no parsing, no network) on the same workload
// the client sends, so book-only vs full-server numbers are comparable.
//
//   bench_book [num_ops] [seed]
//
// Pass 1 times the whole run (throughput, no per-op timer overhead).
// Pass 2 times each op individually for p50/p99; steady_clock calls add some
// tens of ns per op, so read pass-2 latencies as an upper bound.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "orderbook/workload.hpp"

using namespace std;
using namespace ob;

static void apply(OrderBook& book, const Command& c) {
    if (const auto* a = get_if<AddCmd>(&c)) book.add_limit(a->id, a->side, a->price, a->qty);
    else book.cancel(get<CancelCmd>(c).id);
}

int main(int argc, char** argv) {
    const long n = argc > 1 ? atol(argv[1]) : 1000000;
    const unsigned long seed = argc > 2 ? strtoul(argv[2], nullptr, 10) : 12345;

    vector<Command> cmds;
    cmds.reserve(static_cast<size_t>(n));
    Workload w(seed);
    for (long i = 0; i < n; ++i) cmds.push_back(w.next());

    using clock = chrono::steady_clock;

    {
        OrderBook book;
        const auto t0 = clock::now();
        for (const Command& c : cmds) apply(book, c);
        const double secs = chrono::duration<double>(clock::now() - t0).count();
        cout << "throughput: " << static_cast<long>(n / secs) << " ops/s over " << n
             << " ops (" << secs << " s)\n";
    }
    {
        OrderBook book;
        vector<double> ns;
        ns.reserve(cmds.size());
        for (const Command& c : cmds) {
            const auto s = clock::now();
            apply(book, c);
            ns.push_back(chrono::duration<double, nano>(clock::now() - s).count());
        }
        sort(ns.begin(), ns.end());
        auto pct = [&](double p) { return ns[static_cast<size_t>(p * (ns.size() - 1))]; };
        cout << "latency ns: p50=" << pct(0.50) << " p99=" << pct(0.99)
             << " p99.9=" << pct(0.999) << " max=" << ns.back() << "\n";
    }
    return 0;
}
