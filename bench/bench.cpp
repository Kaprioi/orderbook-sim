// Benchmarks:
//   1. Matching engine throughput on one thread (orders per second).
//   2. A Monte Carlo slippage study: how much slippage a market buy pays as it gets bigger.
//      Every trial is an independent simulation, so trials run in parallel across all cores
//      with no shared state. The study runs once on 1 thread and once on N threads to show
//      the speedup.
//
// Usage: ./build/orderbook_bench [trials-per-size] [threads]
#include "orderbook/Simulation.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

using namespace ob;
using Clock = std::chrono::steady_clock;

static double seconds(Clock::time_point since) { return std::chrono::duration<double>(Clock::now() - since).count(); }

// ---------------------------------------------------------------- 1. engine throughput

static void engineThroughput() {
    constexpr int kOrders = 2'000'000;
    struct Pending { Side side; OrderType type; Qty qty; Price price; bool cancel; };
    std::mt19937 rng(42);
    std::normal_distribution<double> offset(0, 8);
    std::uniform_real_distribution<double> unit(0, 1);
    std::vector<Pending> flow;
    flow.reserve(kOrders);
    for (int i = 0; i < kOrders; ++i) {
        const Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
        const bool market = unit(rng) < 0.15;
        const Price price = 10000 - direction(side) * static_cast<Price>(std::abs(offset(rng))) + (unit(rng) < 0.1 ? direction(side) * 3 : 0);
        flow.push_back({side, market ? OrderType::Market : OrderType::Limit, 10 + static_cast<Qty>(unit(rng) * 200), price, unit(rng) < 0.3});
    }

    OrderBook book;
    std::vector<OrderId> live;
    live.reserve(kOrders);
    long fills = 0, cancels = 0;
    book.onFill([&](const Fill&) { ++fills; });
    const auto start = Clock::now();
    for (const Pending& p : flow) {
        if (p.cancel && !live.empty()) {
            const std::size_t pick = static_cast<std::size_t>(p.qty * 7919) % live.size();
            cancels += book.cancel(live[pick]);
            live[pick] = live.back();
            live.pop_back();
            continue;
        }
        const ExecutionReport r = book.submit(1 + (p.qty % 50), p.side, p.type, p.qty, p.price);
        if (r.rested) live.push_back(r.id);
    }
    const double t = seconds(start);
    std::printf("Matching engine, 1 thread\n");
    std::printf("  %s messages in %.2f s  ->  %.2f million messages/s  (%ld fills, %ld cancels, %zu resting at end)\n\n",
                "2,000,000", t, kOrders / t / 1e6, fills, cancels, book.orderCount());
}

// ---------------------------------------------------------------- 2. parallel slippage study

struct Trial { Qty size; unsigned seed; double bps = 0; int levels = 0; };

static void runTrial(Trial& t) {
    SimConfig cfg;
    cfg.seed = t.seed;
    Simulation sim(cfg);
    for (int i = 0; i < 300; ++i) sim.step();  // let the market evolve to a random state
    const ExecutionReport r = sim.submitUser(Side::Buy, OrderType::Market, t.size);
    t.bps = r.slippageBps();
    t.levels = r.levelsSwept;
}

// Hands out trial indices from a shared atomic counter; each thread only writes its own trials.
static double runAll(std::vector<Trial>& trials, unsigned threads) {
    std::atomic<std::size_t> next{0};
    auto work = [&] {
        for (std::size_t i = next++; i < trials.size(); i = next++) runTrial(trials[i]);
    };
    const auto start = Clock::now();
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    return seconds(start);
}

int main(int argc, char** argv) {
    const int trialsPerSize = argc > 1 ? std::max(1, std::atoi(argv[1])) : 150;
    const unsigned threads = argc > 2 ? static_cast<unsigned>(std::max(1, std::atoi(argv[2]))) : std::max(1u, std::thread::hardware_concurrency());

    engineThroughput();

    const std::vector<Qty> sizes = {100, 250, 500, 1000, 2000, 4000};
    auto makeTrials = [&] {
        std::vector<Trial> trials;
        unsigned seed = 1000;
        for (Qty s : sizes)
            for (int i = 0; i < trialsPerSize; ++i) trials.push_back({s, seed++});
        return trials;
    };

    std::printf("Slippage study: %d independent simulations per order size, %zu sizes\n", trialsPerSize, sizes.size());
    std::vector<Trial> serial = makeTrials(), parallel = makeTrials();
    const double tSerial = runAll(serial, 1);
    const double tParallel = runAll(parallel, threads);

    bool same = true;  // same seeds must give the same answers no matter how many threads ran them
    for (std::size_t i = 0; i < serial.size(); ++i) same &= serial[i].bps == parallel[i].bps;

    std::printf("  1 thread:   %.2f s\n  %u threads: %.2f s  ->  %.1fx faster%s\n\n", tSerial, threads, tParallel, tSerial / tParallel,
                same ? "  (identical results)" : "  (RESULTS DIFFER)");

    std::printf("  %-10s %12s %12s %14s\n", "order size", "avg bps", "p95 bps", "avg levels");
    for (Qty s : sizes) {
        std::vector<double> bps;
        double levels = 0;
        for (const Trial& t : parallel)
            if (t.size == s) { bps.push_back(t.bps); levels += t.levels; }
        std::sort(bps.begin(), bps.end());
        double avg = 0;
        for (double b : bps) avg += b;
        avg /= static_cast<double>(bps.size());
        std::printf("  %-10lld %12.2f %12.2f %14.1f\n", static_cast<long long>(s), avg, bps[bps.size() * 95 / 100], levels / static_cast<double>(bps.size()));
    }
    return same ? 0 : 1;
}
