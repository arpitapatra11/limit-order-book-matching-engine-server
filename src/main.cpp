#include <iostream>
#include <string>
#include <vector>

#include "orderbook/order_book.hpp"

using namespace std;

using namespace ob;

static void print_trades(const vector<Trade> &trades)
{
    for (const Trade &t : trades)
    {
        cout << "  TRADE taker=" << t.taker_id << " maker=" << t.maker_id
             << " price=" << t.price << " qty=" << t.qty << "\n";
    }
}

int main()
{
    OrderBook book;

    cout << "Add sell 10 @ 101 (id 1) and sell 5 @ 102 (id 2)\n";
    book.add_limit(1, Side::Sell, 101, 10);
    book.add_limit(2, Side::Sell, 102, 5);

    cout << "Add buy 12 @ 102 (id 3): sweeps two levels\n";
    print_trades(book.add_limit(3, Side::Buy, 102, 12));

    cout << "Best bid: " << (book.best_bid() ? to_string(*book.best_bid()) : "none")
         << ", best ask: " << (book.best_ask() ? to_string(*book.best_ask()) : "none")
         << "\n";
    cout << "Cancel id 2: " << (book.cancel(2) ? "ok" : "not found") << "\n";
    cout << "Resting orders: " << book.order_count() << "\n";
    return 0;
}
