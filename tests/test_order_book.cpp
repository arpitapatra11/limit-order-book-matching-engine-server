// Dependency-free tests. Swap in GoogleTest later if you like.
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

#include "orderbook/order_book.hpp"

using namespace std;

using namespace ob;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            cerr << __FILE__ << ":" << __LINE__                         \
                      << " CHECK failed: " #cond "\n";                       \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Must live in namespace ob so vector's == finds them (ADL).
namespace ob {
inline bool operator==(const Trade& a, const Trade& b) {
    return a.taker_id == b.taker_id && a.maker_id == b.maker_id &&
           a.price == b.price && a.qty == b.qty;
}
inline bool operator==(const Order& a, const Order& b) {
    return a.id == b.id && a.side == b.side && a.price == b.price && a.qty == b.qty;
}
}  // namespace ob

// ---------------------------------------------------------------- unit tests

static void test_no_cross_rests() {
    OrderBook b;
    CHECK(b.add_limit(1, Side::Buy, 99, 10).empty());
    CHECK(b.add_limit(2, Side::Sell, 101, 10).empty());
    CHECK(b.best_bid() == 99);
    CHECK(b.best_ask() == 101);
    CHECK(b.order_count() == 2);
}

static void test_full_fill() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    auto t = b.add_limit(2, Side::Buy, 100, 10);
    CHECK(t.size() == 1);
    CHECK(t[0] == (Trade{2, 1, 100, 10}));
    CHECK(b.order_count() == 0);
    CHECK(!b.best_ask().has_value());
}

static void test_partial_fill_rests_remainder() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 4);
    auto t = b.add_limit(2, Side::Buy, 100, 10);
    CHECK(t.size() == 1 && t[0].qty == 4);
    CHECK(b.best_bid() == 100);
    CHECK(b.quantity_at(Side::Buy, 100) == 6);
    CHECK(!b.best_ask().has_value());
}

static void test_partial_fill_of_resting_order() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 10);
    auto t = b.add_limit(2, Side::Buy, 100, 3);
    CHECK(t.size() == 1 && t[0].qty == 3);
    CHECK(b.quantity_at(Side::Sell, 100) == 7);
    CHECK(b.order_count() == 1);
}

static void test_multi_level_sweep_trades_at_maker_price() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 101, 10);
    b.add_limit(2, Side::Sell, 102, 5);
    auto t = b.add_limit(3, Side::Buy, 105, 12);  // willing to pay up to 105
    CHECK(t.size() == 2);
    CHECK(t[0] == (Trade{3, 1, 101, 10}));        // price improvement: 101, not 105
    CHECK(t[1] == (Trade{3, 2, 102, 2}));
    CHECK(b.quantity_at(Side::Sell, 102) == 3);
    CHECK(!b.best_bid().has_value());
}

static void test_price_priority() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 102, 5);
    b.add_limit(2, Side::Sell, 101, 5);  // better price arrives later
    auto t = b.add_limit(3, Side::Buy, 102, 5);
    CHECK(t.size() == 1 && t[0].maker_id == 2);
}

static void test_time_priority_within_level() {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 5);
    b.add_limit(2, Side::Buy, 100, 5);
    auto t = b.add_limit(3, Side::Sell, 100, 7);
    CHECK(t.size() == 2);
    CHECK(t[0].maker_id == 1 && t[0].qty == 5);  // oldest first
    CHECK(t[1].maker_id == 2 && t[1].qty == 2);
}

static void test_sell_side_sweep() {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 5);
    b.add_limit(2, Side::Buy, 99, 5);
    auto t = b.add_limit(3, Side::Sell, 98, 8);
    CHECK(t.size() == 2);
    CHECK(t[0] == (Trade{3, 1, 100, 5}));
    CHECK(t[1] == (Trade{3, 2, 99, 3}));
}

static void test_cancel() {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 5);
    b.add_limit(2, Side::Buy, 100, 5);
    CHECK(b.cancel(1));
    CHECK(b.quantity_at(Side::Buy, 100) == 5);
    CHECK(!b.cancel(1));    // already cancelled
    CHECK(!b.cancel(999));  // never existed
    CHECK(b.cancel(2));
    CHECK(!b.best_bid().has_value());  // empty level removed
    CHECK(b.order_count() == 0);
}

static void test_cancelled_order_is_not_matched() {
    OrderBook b;
    b.add_limit(1, Side::Sell, 100, 5);
    b.add_limit(2, Side::Sell, 100, 5);
    b.cancel(1);
    auto t = b.add_limit(3, Side::Buy, 100, 5);
    CHECK(t.size() == 1 && t[0].maker_id == 2);
}

static void test_invalid_input() {
    OrderBook b;
    b.add_limit(1, Side::Buy, 100, 5);
    bool dup = false, zero = false;
    try { b.add_limit(1, Side::Buy, 100, 5); } catch (const invalid_argument&) { dup = true; }
    try { b.add_limit(2, Side::Buy, 100, 0); } catch (const invalid_argument&) { zero = true; }
    CHECK(dup);
    CHECK(zero);
    CHECK(b.order_count() == 1);
}

// ------------------------------------------- randomized test vs naive reference

// Deliberately simple and slow: one flat vector, linear scans, explicit
// sequence numbers. If the fast book ever disagrees with this, the fast book
// has a bug.
class ReferenceBook {
public:
    vector<Trade> add(OrderId id, Side side, Price price, Quantity qty) {
        vector<Trade> trades;
        while (qty > 0) {
            int best = -1;
            for (size_t i = 0; i < orders_.size(); ++i) {
                const Order& r = orders_[i].o;
                if (r.side == side) continue;
                const bool crosses = side == Side::Buy ? r.price <= price : r.price >= price;
                if (!crosses) continue;
                if (best < 0 || better(orders_[i], orders_[best])) best = static_cast<int>(i);
            }
            if (best < 0) break;
            Order& r = orders_[best].o;
            const Quantity fill = min(qty, r.qty);
            trades.push_back(Trade{id, r.id, r.price, fill});
            qty -= fill;
            r.qty -= fill;
            if (r.qty == 0) orders_.erase(orders_.begin() + best);
        }
        if (qty > 0) orders_.push_back({Order{id, side, price, qty}, seq_++});
        return trades;
    }

    bool cancel(OrderId id) {
        for (size_t i = 0; i < orders_.size(); ++i) {
            if (orders_[i].o.id == id) {
                orders_.erase(orders_.begin() + i);
                return true;
            }
        }
        return false;
    }

    vector<Order> resting(Side side) const {
        vector<Entry> v;
        for (const Entry& e : orders_) if (e.o.side == side) v.push_back(e);
        sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return better(a, b); });
        vector<Order> out;
        for (const Entry& e : v) out.push_back(e.o);
        return out;
    }

    size_t size() const { return orders_.size(); }

private:
    struct Entry { Order o; uint64_t seq; };

    // a has priority over b (same side assumed).
    static bool better(const Entry& a, const Entry& b) {
        if (a.o.price != b.o.price)
            return a.o.side == Side::Sell ? a.o.price < b.o.price : a.o.price > b.o.price;
        return a.seq < b.seq;
    }

    vector<Entry> orders_;
    uint64_t seq_ = 0;
};

static void test_randomized_against_reference() {
    mt19937_64 rng(12345);
    OrderBook fast;
    ReferenceBook ref;
    OrderId next_id = 1;

    const int kOps = 60000;
    for (int op = 0; op < kOps; ++op) {
        if (next_id > 1 && rng() % 100 < 35) {
            // Cancel a random id: may be live, filled, or already cancelled.
            const OrderId id = 1 + rng() % (next_id - 1);
            CHECK(fast.cancel(id) == ref.cancel(id));
        } else {
            const OrderId id = next_id++;
            const Side side = rng() % 2 ? Side::Buy : Side::Sell;
            const Price price = 95 + static_cast<Price>(rng() % 11);  // 95..105, many crosses
            const Quantity qty = 1 + rng() % 20;
            auto a = fast.add_limit(id, side, price, qty);
            auto b = ref.add(id, side, price, qty);
            CHECK(a.size() == b.size());
            if (a.size() == b.size()) {
                for (size_t i = 0; i < a.size(); ++i) CHECK(a[i] == b[i]);
            }
        }
        if (op % 50 == 0) {
            CHECK(fast.order_count() == ref.size());
            CHECK(fast.resting_orders(Side::Buy) == ref.resting(Side::Buy));
            CHECK(fast.resting_orders(Side::Sell) == ref.resting(Side::Sell));
        }
        if (g_failures > 20) return;  // don't spam
    }
    CHECK(fast.resting_orders(Side::Buy) == ref.resting(Side::Buy));
    CHECK(fast.resting_orders(Side::Sell) == ref.resting(Side::Sell));
}

#define RUN(test)                                              \
    do {                                                       \
        const int before = g_failures;                         \
        test();                                                \
        cout << (g_failures == before ? "[ ok ] " : "[FAIL] ") << #test << "\n"; \
    } while (0)

int main() {
    RUN(test_no_cross_rests);
    RUN(test_full_fill);
    RUN(test_partial_fill_rests_remainder);
    RUN(test_partial_fill_of_resting_order);
    RUN(test_multi_level_sweep_trades_at_maker_price);
    RUN(test_price_priority);
    RUN(test_time_priority_within_level);
    RUN(test_sell_side_sweep);
    RUN(test_cancel);
    RUN(test_cancelled_order_is_not_matched);
    RUN(test_invalid_input);
    RUN(test_randomized_against_reference);

    if (g_failures) {
        cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    cout << "All tests passed\n";
    return EXIT_SUCCESS;
}
