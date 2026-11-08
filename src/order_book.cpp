#include "orderbook/order_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

using namespace std;

namespace ob {

void OrderBook::match(Order& incoming, vector<Trade>& trades) {
    const bool buying = incoming.side == Side::Buy;
    SideBook& opposite = buying ? asks_ : bids_;

    while (incoming.qty > 0 && !opposite.empty()) {
        // Best opposite level: lowest ask for a buyer, highest bid for a seller.
        auto level_it = buying ? opposite.begin() : prev(opposite.end());
        const Price level_price = level_it->first;

        const bool crosses = buying ? level_price <= incoming.price
                                    : level_price >= incoming.price;
        if (!crosses) break;

        Level& level = level_it->second;
        while (incoming.qty > 0 && !level.empty()) {
            Order& resting = level.front();  // oldest order = time priority
            const Quantity fill = min(incoming.qty, resting.qty);

            trades.push_back(Trade{incoming.id, resting.id, level_price, fill});
            incoming.qty -= fill;
            resting.qty  -= fill;

            if (resting.qty == 0) {
                index_.erase(resting.id);
                level.pop_front();
            }
        }
        if (level.empty()) opposite.erase(level_it);
    }
}

vector<Trade> OrderBook::add_limit(OrderId id, Side side, Price price, Quantity qty) {
    if (qty == 0) throw invalid_argument("quantity must be positive");
    if (index_.count(id)) throw invalid_argument("duplicate order id");

    vector<Trade> trades;
    Order incoming{id, side, price, qty};
    match(incoming, trades);

    if (incoming.qty > 0) {
        SideBook& own = side == Side::Buy ? bids_ : asks_;
        auto level_it = own.try_emplace(price).first;
        Level& level = level_it->second;
        level.push_back(incoming);
        index_.emplace(id, Locator{side, level_it, prev(level.end())});
    }
    return trades;
}

bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;

    const Locator loc = it->second;
    loc.level->second.erase(loc.order);
    if (loc.level->second.empty()) {
        (loc.side == Side::Buy ? bids_ : asks_).erase(loc.level);
    }
    index_.erase(it);
    return true;
}

optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return nullopt;
    return bids_.rbegin()->first;
}

optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return nullopt;
    return asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    const SideBook& book = side == Side::Buy ? bids_ : asks_;
    auto it = book.find(price);
    if (it == book.end()) return 0;
    Quantity total = 0;
    for (const Order& o : it->second) total += o.qty;
    return total;
}

vector<Order> OrderBook::resting_orders(Side side) const {
    vector<Order> out;
    auto append = [&out](const Level& level) {
        out.insert(out.end(), level.begin(), level.end());
    };
    if (side == Side::Buy) {
        for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) append(it->second);
    } else {
        for (auto it = asks_.begin(); it != asks_.end(); ++it) append(it->second);
    }
    return out;
}

}  // namespace ob
