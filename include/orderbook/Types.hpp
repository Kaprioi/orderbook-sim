#pragma once

#include <cstdint>
#include <vector>

namespace ob {

// Prices are integer ticks (1 tick = $0.01) so matching never touches floating point.
using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::uint64_t;
using TraderId = std::int32_t;

constexpr double kTickSize = 0.01;
inline double toDollars(double ticks) { return ticks * kTickSize; }

enum class Side { Buy, Sell };
enum class OrderType { Limit, Market };

inline Side opposite(Side s) { return s == Side::Buy ? Side::Sell : Side::Buy; }
inline int direction(Side s) { return s == Side::Buy ? 1 : -1; }
inline const char* toString(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }
inline const char* toString(OrderType t) { return t == OrderType::Limit ? "LIMIT" : "MARKET"; }

struct Order {
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price price = 0;        // limit price; unused for market orders
    Qty quantity = 0;       // original size
    Qty remaining = 0;      // still open
    std::uint64_t seq = 0;  // arrival sequence, for time priority
};

// One match between a resting (maker) order and an incoming (taker) order.
struct Fill {
    OrderId makerOrder = 0;
    OrderId takerOrder = 0;
    TraderId maker = 0;
    TraderId taker = 0;
    Side takerSide = Side::Buy;
    Price price = 0;
    Qty quantity = 0;
    std::uint64_t seq = 0;
};

// What happened to one submitted order, including how much slippage it paid.
struct ExecutionReport {
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    OrderType type = OrderType::Market;
    Qty requested = 0;
    Qty filled = 0;
    Qty rested = 0;           // left on the book (limit orders)
    Qty unfilled = 0;         // dropped because the book ran out (market orders)
    Price limit = 0;
    double averagePrice = 0;  // ticks, volume-weighted over fills
    double arrivalMid = 0;    // book mid when the order arrived (0 if one side was empty)
    Price arrivalTouch = 0;   // best opposite price when the order arrived (0 if none)
    int levelsSwept = 0;      // price levels the order traded through
    std::vector<Fill> fills;

    bool hasArrivalMid() const { return arrivalMid > 0; }
    // Slippage is signed so that positive always means "worse for the trader".
    double slippageVsTouchTicks() const {
        return (filled && arrivalTouch) ? direction(side) * (averagePrice - static_cast<double>(arrivalTouch)) : 0.0;
    }
    double slippageVsMidTicks() const {
        return (filled && hasArrivalMid()) ? direction(side) * (averagePrice - arrivalMid) : 0.0;
    }
    double slippageBps() const {
        return hasArrivalMid() ? slippageVsMidTicks() / arrivalMid * 1e4 : 0.0;
    }
    double slippageCost() const {  // dollars paid versus filling everything at the arrival mid
        return toDollars(slippageVsMidTicks()) * static_cast<double>(filled);
    }
};

}  // namespace ob
