#pragma once

#include "orderbook/OrderBook.hpp"

#include <deque>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace ob {

struct Account {
    Qty position = 0;
    double cash = 0;  // dollars
    Qty volume = 0;
    int trades = 0;
    double pnl(double markTicks) const { return cash + static_cast<double>(position) * toDollars(markTicks); }
};

struct MarketMakerParams {
    double halfSpreadTicks = 3;   // distance from the reference price to the first quote
    Qty size = 120;               // size per quote level
    int levels = 3;               // quote levels per side
    int levelSpacingTicks = 2;    // gap between levels
    double skewTicksPer100 = 1.5; // how far quotes lean against inventory, per 100 shares held
    Qty maxInventory = 1500;      // stop adding to a side past this
    double refreshChance = 0.35;  // chance per step to cancel and requote (stale quotes get picked off)
};

class MarketMaker {
public:
    MarketMaker(TraderId id, std::string name, MarketMakerParams params) : id_(id), name_(std::move(name)), params_(params) {}
    void step(OrderBook& book, double fairValue, const Account& account, std::mt19937& rng);
    TraderId id() const { return id_; }
    const std::string& name() const { return name_; }
    MarketMakerParams& params() { return params_; }
    const MarketMakerParams& params() const { return params_; }
    double lastBid() const { return lastBid_; }
    double lastAsk() const { return lastAsk_; }

private:
    TraderId id_;
    std::string name_;
    MarketMakerParams params_;
    double lastBid_ = 0, lastAsk_ = 0;
};

struct SimConfig {
    double startPrice = 10000;      // ticks ($100.00)
    double volatility = 2.0;        // fair value moves, ticks per step (std dev)
    double jumpChance = 0.002;      // chance per step of a news jump
    double jumpSizeTicks = 40;
    double takerRate = 1.2;         // average market orders per step
    double limitRate = 2.0;         // average non-MM limit orders per step
    double informedShare = 0.25;    // share of takers who trade toward fair value
    Qty takerMeanSize = 60;
    int makerCount = 4;
    MarketMakerParams maker;
    unsigned seed = 7;
};

struct PricePoint {
    long step = 0;
    double fair = 0, mid = 0, bid = 0, ask = 0, last = 0;
};

struct SlippageStats {
    int orders = 0;
    double totalBps = 0, worstBps = 0, totalCost = 0;
    double averageBps() const { return orders ? totalBps / orders : 0; }
};

class Simulation {
public:
    static constexpr TraderId kUser = 0;
    static constexpr TraderId kNoise = 1;     // background traders, one shared account
    static constexpr TraderId kInformed = 2;
    static constexpr TraderId kFirstMaker = 10;

    explicit Simulation(SimConfig config = {});
    void reset();
    void step();

    // Orders from the person using the GUI.
    ExecutionReport submitUser(Side side, OrderType type, Qty quantity, Price limit = 0);

    OrderBook& book() { return book_; }
    const OrderBook& book() const { return book_; }
    SimConfig& config() { return config_; }
    std::vector<MarketMaker>& makers() { return makers_; }
    const Account& account(TraderId id) const;
    double fairValue() const { return fair_; }
    double mark() const;  // mid if both sides exist, else last trade, else fair value
    long steps() const { return step_; }
    const std::deque<PricePoint>& history() const { return history_; }
    const std::deque<ExecutionReport>& userReports() const { return userReports_; }
    const SlippageStats& marketSlippage() const { return marketSlippage_; }  // all market orders, every trader
    Qty volume() const { return volume_; }
    void setMakerCount(int count);

private:
    void applyFill(const Fill& fill);
    void backgroundFlow();
    void trackSlippage(const ExecutionReport& r);
    Qty drawSize(double mean);

    SimConfig config_;
    OrderBook book_;
    std::vector<MarketMaker> makers_;
    std::unordered_map<TraderId, Account> accounts_;
    std::deque<PricePoint> history_;
    std::deque<ExecutionReport> userReports_;
    SlippageStats marketSlippage_;
    std::mt19937 rng_;
    double fair_ = 0;
    long step_ = 0;
    Qty volume_ = 0;

    static constexpr std::size_t kHistoryLength = 900;
};

}  // namespace ob
