#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "orderbook/order_book.hpp"

namespace ob {

// Wire protocol: one text command per line, terminated by '\n' (or "\r\n").
//
//   ADD <id> <BUY|SELL> <price> <qty>
//   CANCEL <id>
//
// Every request gets zero or more TRADE lines followed by exactly one
// terminating line, either "OK" or "ERR <reason>":
//
//   TRADE <taker_id> <maker_id> <price> <qty>
//   OK
//   ERR <reason>
//
// Commands are case-insensitive. Ids and quantities are unsigned, price is a
// positive integer tick count, and trailing tokens are an error.

struct AddCmd    { OrderId id; Side side; Price price; Quantity qty; };
struct CancelCmd { OrderId id; };
using Command = std::variant<AddCmd, CancelCmd>;

struct ParseResult {
    std::optional<Command> command;  // set on success
    std::string            error;    // short reason code on failure
};

ParseResult parse_command(std::string_view line);

// Inverse of parse_command (no trailing newline).
std::string format_command(const Command& cmd);

// Applies a command and returns the full reply text (each line ends in '\n').
std::string execute(OrderBook& book, const Command& cmd);

// parse + execute. Malformed input yields "ERR <reason>\n" and never
// touches the book.
std::string handle_line(OrderBook& book, std::string_view line);

}  // namespace ob
