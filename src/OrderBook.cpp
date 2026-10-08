#include "orderbook/OrderBook.hpp"

#include <algorithm>

namespace ob {

namespace {

bool crosses(Side side, Price limit, Price resting) {
    return side == Side::Buy ? resting <= limit : resting >= limit;
}

}  // namespace

ExecutionReport OrderBook::submit(TraderId trader, Side side, OrderType type, Qty quantity, Price limit) {
    ExecutionReport report;
    report.id = nextId_++;
    report.trader = trader;
    report.side = side;
    report.type = type;
    report.requested = quantity;
    report.limit = limit;
    if (auto m = mid()) report.arrivalMid = *m;
    if (auto touch = side == Side::Buy ? bestAsk() : bestBid()) report.arrivalTouch = *touch;

    if (quantity <= 0 || (type == OrderType::Limit && limit <= 0)) return report;

    Order order{report.id, trader, side, type, limit, quantity, quantity, ++seq_};
    if (side == Side::Buy) match(order, asks_, report);
    else match(order, bids_, report);

    report.filled = quantity - order.remaining;
    if (order.remaining > 0) {
        if (type == OrderType::Limit) {
            rest(order);
            report.rested = order.remaining;
        } else {
            report.unfilled = order.remaining;  // market orders never rest
        }
    }
    if (report.filled > 0) {
        double notional = 0;
        for (const Fill& f : report.fills) notional += static_cast<double>(f.price) * static_cast<double>(f.quantity);
        report.averagePrice = notional / static_cast<double>(report.filled);
    }
    return report;
}

template <class Book>
void OrderBook::match(Order& taker, Book& book, ExecutionReport& report) {
    while (taker.remaining > 0 && !book.empty()) {
        auto level = book.begin();
        if (taker.type == OrderType::Limit && !crosses(taker.side, taker.price, level->first)) break;

        Queue& queue = level->second;
        bool tradedHere = false;
        while (taker.remaining > 0 && !queue.empty()) {
            Order& maker = queue.front();
            if (maker.trader == taker.trader) {  // self-trade prevention: pull our own resting order
                index_.erase(maker.id);
                queue.pop_front();
                continue;
            }
            const Qty traded = std::min(taker.remaining, maker.remaining);
            taker.remaining -= traded;
            maker.remaining -= traded;
            tradedHere = true;

            Fill fill{maker.id, taker.id, maker.trader, taker.trader, taker.side, level->first, traded, ++seq_};
            report.fills.push_back(fill);
            if (maker.remaining == 0) {
                index_.erase(maker.id);
                queue.pop_front();
            }
            record(fill);
        }
        if (tradedHere) ++report.levelsSwept;
        if (queue.empty()) book.erase(level);
    }
}

void OrderBook::rest(const Order& order) {
    Queue* queue = nullptr;
    if (order.side == Side::Buy) queue = &bids_[order.price];
    else queue = &asks_[order.price];
    queue->push_back(order);
    index_[order.id] = Location{order.side, order.price, std::prev(queue->end())};
}

void OrderBook::record(const Fill& fill) {
    lastTrade_ = fill.price;
    tape_.push_front(fill);
    if (tape_.size() > kTapeLength) tape_.pop_back();
    if (listener_) listener_(fill);
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Location loc = found->second;
    index_.erase(found);
    auto drop = [&](auto& book) {
        auto level = book.find(loc.price);
        if (level == book.end()) return;
        level->second.erase(loc.it);
        if (level->second.empty()) book.erase(level);
    };
    if (loc.side == Side::Buy) drop(bids_);
    else drop(asks_);
    return true;
}

void OrderBook::cancelAll(TraderId trader) {
    std::vector<OrderId> ids;
    for (const auto& [id, loc] : index_)
        if (loc.it->trader == trader) ids.push_back(id);
    for (OrderId id : ids) cancel(id);
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    index_.clear();
    tape_.clear();
    lastTrade_.reset();
}

ExecutionReport OrderBook::estimateMarket(Side side, Qty quantity) const {
    ExecutionReport report;
    report.side = side;
    report.type = OrderType::Market;
    report.requested = quantity;
    if (auto m = mid()) report.arrivalMid = *m;
    if (auto touch = side == Side::Buy ? bestAsk() : bestBid()) report.arrivalTouch = *touch;

    Qty left = quantity;
    double notional = 0;
    auto walk = [&](const auto& book) {
        for (const auto& [price, queue] : book) {
            if (left <= 0) break;
            Qty available = 0;
            for (const Order& o : queue) available += o.remaining;
            const Qty take = std::min(left, available);
            if (take <= 0) continue;
            notional += static_cast<double>(price) * static_cast<double>(take);
            left -= take;
            ++report.levelsSwept;
        }
    };
    if (side == Side::Buy) walk(asks_);
    else walk(bids_);

    report.filled = quantity - left;
    report.unfilled = left;
    if (report.filled > 0) report.averagePrice = notional / static_cast<double>(report.filled);
    return report;
}

std::optional<Price> OrderBook::bestBid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::bestAsk() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::optional<double> OrderBook::mid() const {
    auto b = bestBid(), a = bestAsk();
    if (!b || !a) return std::nullopt;
    return (static_cast<double>(*b) + static_cast<double>(*a)) / 2.0;
}

std::optional<Price> OrderBook::spread() const {
    auto b = bestBid(), a = bestAsk();
    if (!b || !a) return std::nullopt;
    return *a - *b;
}

std::vector<OrderBook::Level> OrderBook::depth(Side side, std::size_t levels) const {
    std::vector<Level> out;
    auto collect = [&](const auto& book) {
        for (const auto& [price, queue] : book) {
            if (out.size() >= levels) break;
            Level l{price, 0, 0};
            for (const Order& o : queue) {
                l.quantity += o.remaining;
                ++l.orders;
            }
            out.push_back(l);
        }
    };
    if (side == Side::Buy) collect(bids_);
    else collect(asks_);
    return out;
}

Qty OrderBook::totalQuantity(Side side) const {
    Qty total = 0;
    auto sum = [&](const auto& book) {
        for (const auto& [price, queue] : book)
            for (const Order& o : queue) total += o.remaining;
    };
    if (side == Side::Buy) sum(bids_);
    else sum(asks_);
    return total;
}

const Order* OrderBook::find(OrderId id) const {
    auto found = index_.find(id);
    return found == index_.end() ? nullptr : &*found->second.it;
}

std::vector<Order> OrderBook::ordersFor(TraderId trader) const {
    std::vector<Order> out;
    for (const auto& [id, loc] : index_)
        if (loc.it->trader == trader) out.push_back(*loc.it);
    std::sort(out.begin(), out.end(), [](const Order& a, const Order& b) { return a.seq < b.seq; });
    return out;
}

}  // namespace ob
