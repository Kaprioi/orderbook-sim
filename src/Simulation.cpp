#include "orderbook/Simulation.hpp"

#include <algorithm>
#include <cmath>

namespace ob {

// ---------------------------------------------------------------- market maker

void MarketMaker::step(OrderBook& book, double fairValue, const Account& account, std::mt19937& rng) {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    if (!book.ordersFor(id_).empty() && unit(rng) > params_.refreshChance) return;  // keep the old (maybe stale) quotes

    book.cancelAll(id_);

    // Makers don't see the true value: they blend a noisy read of it with the book's own mid.
    std::normal_distribution<double> noise(0.0, 1.5);
    double reference = fairValue + noise(rng);
    if (auto m = book.mid()) reference = 0.6 * reference + 0.4 * *m;

    // Lean against inventory: long makers lower both quotes to sell more and buy less.
    const double skew = params_.skewTicksPer100 * static_cast<double>(account.position) / 100.0;
    const double bid = std::floor(reference - params_.halfSpreadTicks - skew);
    const double ask = std::ceil(reference + params_.halfSpreadTicks - skew);
    lastBid_ = bid;
    lastAsk_ = ask;

    const bool canBuy = account.position < params_.maxInventory;
    const bool canSell = account.position > -params_.maxInventory;
    for (int level = 0; level < params_.levels; ++level) {
        const Price offset = static_cast<Price>(level) * params_.levelSpacingTicks;
        const Qty size = params_.size + params_.size * level / 2;  // deeper levels show more size
        if (canBuy) book.submit(id_, Side::Buy, OrderType::Limit, size, static_cast<Price>(bid) - offset);
        if (canSell) book.submit(id_, Side::Sell, OrderType::Limit, size, static_cast<Price>(ask) + offset);
    }
}

// ---------------------------------------------------------------- simulation

Simulation::Simulation(SimConfig config) : config_(config) { reset(); }

void Simulation::reset() {
    book_.clear();
    accounts_.clear();
    history_.clear();
    userReports_.clear();
    marketSlippage_ = {};
    rng_.seed(config_.seed);
    fair_ = config_.startPrice;
    step_ = 0;
    volume_ = 0;
    book_.onFill([this](const Fill& f) { applyFill(f); });
    setMakerCount(config_.makerCount);
    for (int i = 0; i < 20; ++i) step();  // let the makers build a book before anyone looks
}

void Simulation::setMakerCount(int count) {
    count = std::clamp(count, 0, 8);
    while (static_cast<int>(makers_.size()) > count) {
        book_.cancelAll(makers_.back().id());
        makers_.pop_back();
    }
    static const char* names[] = {"Alder", "Birch", "Cedar", "Dogwood", "Elm", "Fir", "Hazel", "Juniper"};
    while (static_cast<int>(makers_.size()) < count) {
        const int i = static_cast<int>(makers_.size());
        MarketMakerParams p = config_.maker;
        p.halfSpreadTicks += i * 0.75;  // give each maker its own personality
        p.size = config_.maker.size * (4 + i % 3) / 5;
        makers_.emplace_back(kFirstMaker + i, names[i], p);
    }
    config_.makerCount = count;
}

const Account& Simulation::account(TraderId id) const {
    static const Account empty;
    auto found = accounts_.find(id);
    return found == accounts_.end() ? empty : found->second;
}

double Simulation::mark() const {
    if (auto m = book_.mid()) return *m;
    if (auto l = book_.lastTrade()) return static_cast<double>(*l);
    return fair_;
}

void Simulation::applyFill(const Fill& f) {
    const TraderId buyer = f.takerSide == Side::Buy ? f.taker : f.maker;
    const TraderId seller = f.takerSide == Side::Buy ? f.maker : f.taker;
    const double notional = toDollars(static_cast<double>(f.price)) * static_cast<double>(f.quantity);
    Account& b = accounts_[buyer];
    Account& s = accounts_[seller];
    b.position += f.quantity;
    b.cash -= notional;
    s.position -= f.quantity;
    s.cash += notional;
    b.volume += f.quantity;
    s.volume += f.quantity;
    ++b.trades;
    ++s.trades;
    volume_ += f.quantity;
}

Qty Simulation::drawSize(double mean) {
    std::exponential_distribution<double> size(1.0 / std::max(1.0, mean));
    return std::max<Qty>(1, static_cast<Qty>(std::round(size(rng_) / 10.0)) * 10 + 10);
}

void Simulation::trackSlippage(const ExecutionReport& r) {
    if (r.type != OrderType::Market || r.filled == 0 || !r.hasArrivalMid()) return;
    ++marketSlippage_.orders;
    marketSlippage_.totalBps += r.slippageBps();
    marketSlippage_.worstBps = std::max(marketSlippage_.worstBps, r.slippageBps());
    marketSlippage_.totalCost += r.slippageCost();
}

void Simulation::backgroundFlow() {
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    const double m = mark();

    // Market orders: noise traders pick a random side; informed traders trade toward fair value.
    std::poisson_distribution<int> takers(config_.takerRate);
    for (int n = takers(rng_); n > 0; --n) {
        const bool informed = unit(rng_) < config_.informedShare;
        Side side = unit(rng_) < 0.5 ? Side::Buy : Side::Sell;
        TraderId who = kNoise;
        if (informed) {
            const double gap = fair_ - m;
            if (std::abs(gap) < 5.0) continue;  // not worth paying the spread for
            side = gap > 0 ? Side::Buy : Side::Sell;
            who = kInformed;
        }
        trackSlippage(book_.submit(who, side, OrderType::Market, drawSize(static_cast<double>(config_.takerMeanSize) * (informed ? 1.6 : 1.0))));
    }

    // Passive limit orders from everyone else, scattered a few ticks around the mid.
    std::poisson_distribution<int> limits(config_.limitRate);
    std::geometric_distribution<int> distance(0.25);
    for (int n = limits(rng_); n > 0; --n) {
        const Side side = unit(rng_) < 0.5 ? Side::Buy : Side::Sell;
        const Price away = 1 + distance(rng_);
        const Price price = static_cast<Price>(std::round(m)) - direction(side) * away;
        if (price > 0) book_.submit(kNoise, side, OrderType::Limit, drawSize(static_cast<double>(config_.takerMeanSize) * 0.8), price);
    }

    // Old background orders get cancelled so the book doesn't silt up.
    for (const Order& o : book_.ordersFor(kNoise))
        if (std::abs(static_cast<double>(o.price) - m) > 30 || unit(rng_) < 0.03) book_.cancel(o.id);
}

void Simulation::step() {
    ++step_;
    std::normal_distribution<double> move(0.0, config_.volatility);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    fair_ += move(rng_);
    if (unit(rng_) < config_.jumpChance) fair_ += (unit(rng_) < 0.5 ? -1 : 1) * config_.jumpSizeTicks;
    fair_ = std::max(fair_, 100.0);

    // Makers act in a random order each step so none of them always gets there first.
    std::vector<std::size_t> order(makers_.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::shuffle(order.begin(), order.end(), rng_);
    for (std::size_t i : order) makers_[i].step(book_, fair_, accounts_[makers_[i].id()], rng_);

    backgroundFlow();

    PricePoint p;
    p.step = step_;
    p.fair = fair_;
    p.mid = mark();
    p.bid = book_.bestBid() ? static_cast<double>(*book_.bestBid()) : p.mid;
    p.ask = book_.bestAsk() ? static_cast<double>(*book_.bestAsk()) : p.mid;
    p.last = book_.lastTrade() ? static_cast<double>(*book_.lastTrade()) : p.mid;
    history_.push_back(p);
    if (history_.size() > kHistoryLength) history_.pop_front();
}

ExecutionReport Simulation::submitUser(Side side, OrderType type, Qty quantity, Price limit) {
    ExecutionReport r = book_.submit(kUser, side, type, quantity, limit);
    trackSlippage(r);
    userReports_.push_front(r);
    if (userReports_.size() > 50) userReports_.pop_back();
    return r;
}

}  // namespace ob
