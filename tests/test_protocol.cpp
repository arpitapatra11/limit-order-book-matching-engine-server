// Protocol tests: parser edge cases, reply formatting, and book integrity
// after bad input. Same dependency-free style as test_order_book.cpp.
#include <cstdlib>
#include <iostream>
#include <string>

#include "orderbook/protocol.hpp"
#include "orderbook/workload.hpp"

using namespace std;
using namespace ob;

static int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            cerr << __FILE__ << ":" << __LINE__ << " CHECK failed: " #cond "\n"; \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static bool rejects(const string& line, const string& reason) {
    const ParseResult r = parse_command(line);
    return !r.command.has_value() && r.error == reason;
}

static void test_parse_valid() {
    auto r = parse_command("ADD 7 BUY 101 10");
    CHECK(r.command.has_value());
    const auto* a = r.command ? get_if<AddCmd>(&*r.command) : nullptr;
    CHECK(a && a->id == 7 && a->side == Side::Buy && a->price == 101 && a->qty == 10);

    r = parse_command("CANCEL 7");
    const auto* c = r.command ? get_if<CancelCmd>(&*r.command) : nullptr;
    CHECK(c && c->id == 7);

    // case-insensitive, extra spaces/tabs, CRLF
    r = parse_command("  add\t8   sell 5  3\r\n");
    const auto* a2 = r.command ? get_if<AddCmd>(&*r.command) : nullptr;
    CHECK(a2 && a2->id == 8 && a2->side == Side::Sell && a2->price == 5 && a2->qty == 3);
}

static void test_parse_malformed() {
    CHECK(rejects("", "empty_command"));
    CHECK(rejects("   \t ", "empty_command"));
    CHECK(rejects("\r\n", "empty_command"));
    CHECK(rejects("FOO 1 2", "unknown_command"));
    CHECK(rejects("ADDX 1 BUY 1 1", "unknown_command"));
    CHECK(rejects("ADD", "wrong_arg_count"));
    CHECK(rejects("ADD 1 BUY 100", "wrong_arg_count"));
    CHECK(rejects("ADD 1 BUY 100 5 extra", "wrong_arg_count"));
    CHECK(rejects("CANCEL", "wrong_arg_count"));
    CHECK(rejects("CANCEL 1 2", "wrong_arg_count"));
    CHECK(rejects("ADD abc BUY 100 5", "bad_id"));
    CHECK(rejects("ADD -1 BUY 100 5", "bad_id"));
    CHECK(rejects("ADD 1 HOLD 100 5", "bad_side"));
    CHECK(rejects("ADD 1 BUY 0 5", "bad_price"));
    CHECK(rejects("ADD 1 BUY -100 5", "bad_price"));
    CHECK(rejects("ADD 1 BUY 10.5 5", "bad_price"));
    CHECK(rejects("ADD 1 BUY 100 -5", "bad_qty"));
    CHECK(rejects("ADD 1 BUY 100 0", "bad_qty"));
    CHECK(rejects("ADD 1 BUY 100 5x", "bad_qty"));
    CHECK(rejects("ADD 1 BUY 100 +5", "bad_qty"));
    CHECK(rejects("ADD 1 BUY 100 99999999999999999999999", "bad_qty"));  // overflow
    CHECK(rejects("CANCEL -1", "bad_id"));
    CHECK(rejects("CANCEL x", "bad_id"));
}

static void test_replies() {
    OrderBook b;
    CHECK(handle_line(b, "ADD 1 SELL 101 10") == "OK\n");
    CHECK(handle_line(b, "ADD 2 SELL 102 5") == "OK\n");
    CHECK(handle_line(b, "ADD 3 BUY 102 12") == "TRADE 3 1 101 10\nTRADE 3 2 102 2\nOK\n");
    CHECK(b.quantity_at(Side::Sell, 102) == 3);
    CHECK(handle_line(b, "CANCEL 2") == "OK\n");
    CHECK(handle_line(b, "CANCEL 2") == "ERR not_found\n");
    CHECK(handle_line(b, "CANCEL 999") == "ERR not_found\n");
    CHECK(b.order_count() == 0);
}

static void test_book_errors_become_err_lines() {
    OrderBook b;
    CHECK(handle_line(b, "ADD 1 BUY 100 5") == "OK\n");
    const string dup = handle_line(b, "ADD 1 BUY 100 5");
    CHECK(dup.rfind("ERR ", 0) == 0 && dup.back() == '\n');
    CHECK(b.order_count() == 1);
}

static void test_bad_input_never_touches_book() {
    OrderBook b;
    handle_line(b, "ADD 1 BUY 100 5");
    const char* bad[] = {"", "ADD", "ADD 2 BUY 100 -5", "ADD 2 BUY 100 0", "ADD 2 BUY 0 5",
                         "NOPE", "CANCEL", "CANCEL -3", "ADD 2 SIDEWAYS 100 5"};
    for (const char* line : bad) {
        const string reply = handle_line(b, line);
        CHECK(reply.rfind("ERR", 0) == 0);
    }
    CHECK(b.order_count() == 1);
    CHECK(b.quantity_at(Side::Buy, 100) == 5);
}

static void test_format_parse_roundtrip() {
    Workload w(99);
    for (int i = 0; i < 2000; ++i) {
        const Command c = w.next();
        const string line = format_command(c);
        const ParseResult r = parse_command(line);
        CHECK(r.command.has_value());
        if (r.command) CHECK(format_command(*r.command) == line);
    }
}

// Whole path (text in, text out) must match driving the book directly.
static void test_protocol_matches_direct_calls() {
    OrderBook via_text, direct;
    Workload w(7);
    for (int i = 0; i < 20000; ++i) {
        const Command c = w.next();
        const string reply = execute(via_text, c);
        string expected;
        if (const auto* a = get_if<AddCmd>(&c)) {
            for (const Trade& t : direct.add_limit(a->id, a->side, a->price, a->qty)) {
                expected += "TRADE " + to_string(t.taker_id) + " " + to_string(t.maker_id) + " " +
                            to_string(t.price) + " " + to_string(t.qty) + "\n";
            }
            expected += "OK\n";
        } else {
            expected = direct.cancel(get<CancelCmd>(c).id) ? "OK\n" : "ERR not_found\n";
        }
        CHECK(reply == expected);
        if (g_failures > 20) return;
    }
    CHECK(via_text.order_count() == direct.order_count());
}

#define RUN(test)                                                                 \
    do {                                                                          \
        const int before = g_failures;                                            \
        test();                                                                   \
        cout << (g_failures == before ? "[ ok ] " : "[FAIL] ") << #test << "\n"; \
    } while (0)

int main() {
    RUN(test_parse_valid);
    RUN(test_parse_malformed);
    RUN(test_replies);
    RUN(test_book_errors_become_err_lines);
    RUN(test_bad_input_never_touches_book);
    RUN(test_format_parse_roundtrip);
    RUN(test_protocol_matches_direct_calls);

    if (g_failures) {
        cerr << g_failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }
    cout << "All protocol tests passed\n";
    return EXIT_SUCCESS;
}
