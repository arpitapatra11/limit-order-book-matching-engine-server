#include "orderbook/protocol.hpp"

#include <cctype>
#include <charconv>
#include <stdexcept>
#include <vector>

namespace ob {
namespace {

std::vector<std::string_view> split(std::string_view s) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        std::size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\t') ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// Whole token must be a number: rejects "", "+5", "5x", "-5" (unsigned), overflow.
template <class T>
bool parse_num(std::string_view s, T& out) {
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

ParseResult fail(const char* reason) { return ParseResult{std::nullopt, reason}; }

}  // namespace

ParseResult parse_command(std::string_view line) {
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.remove_suffix(1);

    const auto tok = split(line);
    if (tok.empty()) return fail("empty_command");

    if (iequals(tok[0], "ADD")) {
        if (tok.size() != 5) return fail("wrong_arg_count");
        AddCmd c{};
        if (!parse_num(tok[1], c.id)) return fail("bad_id");
        if (iequals(tok[2], "BUY"))       c.side = Side::Buy;
        else if (iequals(tok[2], "SELL")) c.side = Side::Sell;
        else return fail("bad_side");
        if (!parse_num(tok[3], c.price) || c.price <= 0) return fail("bad_price");
        if (!parse_num(tok[4], c.qty) || c.qty == 0)     return fail("bad_qty");
        return ParseResult{Command{c}, {}};
    }
    if (iequals(tok[0], "CANCEL")) {
        if (tok.size() != 2) return fail("wrong_arg_count");
        CancelCmd c{};
        if (!parse_num(tok[1], c.id)) return fail("bad_id");
        return ParseResult{Command{c}, {}};
    }
    return fail("unknown_command");
}

std::string format_command(const Command& cmd) {
    if (const auto* a = std::get_if<AddCmd>(&cmd)) {
        return "ADD " + std::to_string(a->id) + (a->side == Side::Buy ? " BUY " : " SELL ") +
               std::to_string(a->price) + " " + std::to_string(a->qty);
    }
    return "CANCEL " + std::to_string(std::get<CancelCmd>(cmd).id);
}

std::string execute(OrderBook& book, const Command& cmd) {
    if (const auto* a = std::get_if<AddCmd>(&cmd)) {
        std::vector<Trade> trades;
        try {
            trades = book.add_limit(a->id, a->side, a->price, a->qty);
        } catch (const std::invalid_argument& e) {
            return std::string("ERR ") + e.what() + "\n";
        }
        std::string out;
        for (const Trade& t : trades) {
            out += "TRADE " + std::to_string(t.taker_id) + " " + std::to_string(t.maker_id) +
                   " " + std::to_string(t.price) + " " + std::to_string(t.qty) + "\n";
        }
        out += "OK\n";
        return out;
    }
    return book.cancel(std::get<CancelCmd>(cmd).id) ? "OK\n" : "ERR not_found\n";
}

std::string handle_line(OrderBook& book, std::string_view line) {
    ParseResult r = parse_command(line);
    if (!r.command) return "ERR " + r.error + "\n";
    return execute(book, *r.command);
}

}  // namespace ob
