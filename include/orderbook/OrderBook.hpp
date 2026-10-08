#pragma once

#include "orderbook/Types.hpp"

#include <deque>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace ob {

// A single-instrument limit order book with price-time priority.
//
// Bids are kept highest-first and asks lowest-first; each price level is a FIFO queue.
// Incoming orders match against the opposite side until they are filled, run out of
// crossing liquidity (limit orders then rest), or empty the book (market orders drop the rest).
// Self-trades are prevented by cancelling the resting order (cancel-resting policy).
class OrderBook {
public:
    struct Level {
        Price price = 0;
        Qty quantity = 0;
        int orders = 0;
    };

    ExecutionReport submit(TraderId trader, Side side, OrderType type, Qty quantity, Price limit = 0);
    bool cancel(OrderId id);
    void cancelAll(TraderId trader);
    void clear();

    // Walks the book as a market order would, without changing anything.
    ExecutionReport estimateMarket(Side side, Qty quantity) const;

    std::optional<Price> bestBid() const;
    std::optional<Price> bestAsk() const;
    std::optional<double> mid() const;
    std::optional<Price> spread() const;

    std::vector<Level> depth(Side side, std::size_t levels) const;
    Qty totalQuantity(Side side) const;
    const Order* find(OrderId id) const;
    std::vector<Order> ordersFor(TraderId trader) const;
    std::size_t orderCount() const { return index_.size(); }

    const std::deque<Fill>& tape() const { return tape_; }
    std::optional<Price> lastTrade() const { return lastTrade_; }

    // Called for every fill, after both orders are updated.
    void onFill(std::function<void(const Fill&)> listener) { listener_ = std::move(listener); }

private:
    using Queue = std::list<Order>;
    using Bids = std::map<Price, Queue, std::greater<Price>>;
    using Asks = std::map<Price, Queue, std::less<Price>>;

    struct Location {
        Side side;
        Price price;
        Queue::iterator it;
    };

    template <class Book>
    void match(Order& taker, Book& book, ExecutionReport& report);
    void rest(const Order& order);
    void record(const Fill& fill);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Location> index_;
    std::deque<Fill> tape_;
    std::optional<Price> lastTrade_;
    std::function<void(const Fill&)> listener_;
    OrderId nextId_ = 1;
    std::uint64_t seq_ = 0;

    static constexpr std::size_t kTapeLength = 400;
};

}  // namespace ob
