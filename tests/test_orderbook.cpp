// Small self-contained tests for the matching engine. Run: ./build/orderbook_tests
#include "orderbook/OrderBook.hpp"
#include "orderbook/Simulation.hpp"

#include <cmath>
#include <cstdio>

using namespace ob;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                      \
        }                                                                    \
    } while (0)
#define NEAR(a, b) CHECK(std::abs((a) - (b)) < 1e-9)

static void priceTimePriority() {
    OrderBook book;
    auto first = book.submit(1, Side::Sell, OrderType::Limit, 100, 10010);
    auto second = book.submit(2, Side::Sell, OrderType::Limit, 100, 10010);
    book.submit(3, Side::Sell, OrderType::Limit, 100, 10005);  // better price, arrives last

    auto r = book.submit(9, Side::Buy, OrderType::Market, 150);
    CHECK(r.fills.size() == 2);
    CHECK(r.fills[0].price == 10005);              // best price first
    CHECK(r.fills[1].makerOrder == first.id);      // then the earliest order at the next level
    CHECK(book.find(first.id)->remaining == 50);
    CHECK(book.find(second.id)->remaining == 100);
}

static void limitCrossesThenRests() {
    OrderBook book;
    book.submit(1, Side::Sell, OrderType::Limit, 50, 10000);
    auto r = book.submit(2, Side::Buy, OrderType::Limit, 80, 10002);
    CHECK(r.filled == 50);
    CHECK(r.rested == 30);
    CHECK(book.bestBid() && *book.bestBid() == 10002);
    CHECK(!book.bestAsk());
}

static void marketSweepSlippage() {
    OrderBook book;
    book.submit(1, Side::Buy, OrderType::Limit, 100, 9998);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 10002);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 10004);
    book.submit(1, Side::Sell, OrderType::Limit, 100, 10010);

    auto estimate = book.estimateMarket(Side::Buy, 250);
    auto r = book.submit(2, Side::Buy, OrderType::Market, 250);
    // 100 @ 10002 + 100 @ 10004 + 50 @ 10010 = average 10004.4
    NEAR(r.averagePrice, 10004.4);
    NEAR(estimate.averagePrice, r.averagePrice);
    CHECK(r.levelsSwept == 3);
    NEAR(r.arrivalMid, 10000.0);
    NEAR(r.slippageVsMidTicks(), 4.4);
    NEAR(r.slippageVsTouchTicks(), 2.4);
    NEAR(r.slippageBps(), 4.4);  // 4.4 ticks on a 10000-tick price is 4.4 bps
}

static void sellSlippageIsPositiveWhenWorse() {
    OrderBook book;
    book.submit(1, Side::Sell, OrderType::Limit, 10, 10002);
    book.submit(1, Side::Buy, OrderType::Limit, 10, 9998);
    book.submit(1, Side::Buy, OrderType::Limit, 10, 9990);
    auto r = book.submit(2, Side::Sell, OrderType::Market, 20);
    NEAR(r.averagePrice, 9994.0);
    NEAR(r.slippageVsMidTicks(), 6.0);
}

static void marketOrderNeverRests() {
    OrderBook book;
    book.submit(1, Side::Sell, OrderType::Limit, 40, 10000);
    auto r = book.submit(2, Side::Buy, OrderType::Market, 100);
    CHECK(r.filled == 40);
    CHECK(r.unfilled == 60);
    CHECK(book.orderCount() == 0);
}

static void cancelAndSelfTrade() {
    OrderBook book;
    auto mine = book.submit(5, Side::Sell, OrderType::Limit, 100, 10000);
    book.submit(6, Side::Sell, OrderType::Limit, 100, 10001);
    CHECK(book.cancel(mine.id));
    CHECK(!book.cancel(mine.id));
    CHECK(*book.bestAsk() == 10001);

    auto resting = book.submit(7, Side::Sell, OrderType::Limit, 100, 10000);
    auto r = book.submit(7, Side::Buy, OrderType::Market, 50);  // would hit its own order
    CHECK(book.find(resting.id) == nullptr);                     // own order pulled instead
    CHECK(r.fills.size() == 1 && r.fills[0].maker == 6);
}

static void simulationConservesShares() {
    Simulation sim;
    for (int i = 0; i < 2000; ++i) sim.step();
    sim.submitUser(Side::Buy, OrderType::Market, 500);
    Qty net = sim.account(Simulation::kUser).position + sim.account(Simulation::kNoise).position +
              sim.account(Simulation::kInformed).position;
    for (auto& m : sim.makers()) net += sim.account(m.id()).position;
    CHECK(net == 0);  // every share bought was sold by someone
    CHECK(sim.book().bestBid() && sim.book().bestAsk());
    CHECK(*sim.book().bestBid() < *sim.book().bestAsk());  // book never stays crossed
}

int main() {
    struct { const char* name; void (*fn)(); } tests[] = {
        {"price-time priority", priceTimePriority},
        {"limit crosses then rests", limitCrossesThenRests},
        {"market sweep slippage", marketSweepSlippage},
        {"sell slippage sign", sellSlippageIsPositiveWhenWorse},
        {"market order never rests", marketOrderNeverRests},
        {"cancel and self-trade prevention", cancelAndSelfTrade},
        {"simulation conserves shares", simulationConservesShares},
    };
    for (auto& t : tests) {
        const int before = failures;
        t.fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf(failures ? "\n%d check(s) failed\n" : "\nall tests passed\n", failures);
    return failures ? 1 : 0;
}
