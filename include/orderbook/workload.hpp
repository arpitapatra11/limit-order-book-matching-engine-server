#pragma once

#include <cstdint>
#include <random>

#include "orderbook/protocol.hpp"

namespace ob {

// Deterministic random order flow, the same mix as the randomized unit test:
// 35% cancels of a random earlier id (may be live, filled or already gone),
// 65% limit orders, side 50/50, price 95..105, qty 1..20.
// Shared by the client and the benchmark so the two measure the same workload.
class Workload {
public:
    explicit Workload(std::uint64_t seed = 12345) : rng_(seed) {}

    Command next() {
        if (next_id_ > 1 && rng_() % 100 < 35) {
            return CancelCmd{1 + rng_() % (next_id_ - 1)};
        }
        const OrderId  id    = next_id_++;
        const Side     side  = rng_() % 2 ? Side::Buy : Side::Sell;
        const Price    price = 95 + static_cast<Price>(rng_() % 11);
        const Quantity qty   = 1 + rng_() % 20;
        return AddCmd{id, side, price, qty};
    }

private:
    std::mt19937_64 rng_;
    OrderId next_id_ = 1;
};

}  // namespace ob
