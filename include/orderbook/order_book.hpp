#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

using OrderId  = std::uint64_t;
using Price    = std::int64_t;   // integer ticks, never floating point
using Quantity = std::uint64_t;

enum class Side { Buy, Sell };

struct Order {
    OrderId  id;
    Side     side;
    Price    price;
    Quantity qty;  // remaining (unfilled) quantity
};

struct Trade {
    OrderId  taker_id;  // the incoming order
    OrderId  maker_id;  // the resting order
    Price    price;     // always the resting (maker) order's price
    Quantity qty;
};

// Limit order book with price-time priority.
//
// Layout:
//   bids_ / asks_ : ordered map  price -> FIFO list of resting orders
//   index_        : order id -> (side, level iterator, order iterator)
//
// Complexity (L = number of non-empty price levels):
//   cancel         : O(1) to locate and unlink the order via index_,
//                    plus amortised O(1) to drop the level if it empties
//   add (no cross) : O(log L) to find or create the price level
//   add (crossing) : O(log L) + O(1) per fill
//
// This is the "correct first" version: plain std containers and heap
// allocation. Profile before optimising (memory pools, flat arrays, etc.).
class OrderBook {
public:
    // Matches against the opposite side, rests any remainder.
    // Throws std::invalid_argument on qty == 0 or an id already resting.
    std::vector<Trade> add_limit(OrderId id, Side side, Price price, Quantity qty);

    // Returns false if the id is not resting (never existed or already filled).
    bool cancel(OrderId id);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity quantity_at(Side side, Price price) const;
    std::size_t order_count() const { return index_.size(); }

    // Resting orders in priority order (best price first, then oldest first).
    std::vector<Order> resting_orders(Side side) const;

private:
    using Level    = std::list<Order>;
    using SideBook = std::map<Price, Level>;

    struct Locator {
        Side                 side;
        SideBook::iterator   level;
        Level::iterator      order;
    };

    void match(Order& incoming, std::vector<Trade>& trades);

    SideBook bids_;  // best bid = highest key (last element)
    SideBook asks_;  // best ask = lowest key (first element)
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace ob
