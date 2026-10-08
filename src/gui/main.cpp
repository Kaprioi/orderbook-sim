// Order book simulator GUI (raylib).
//
// Threads: the simulation runs on its own worker thread so its speed isn't tied to the
// screen's 60 fps. The GUI thread takes the same mutex only while it reads the market and
// queues draw calls; the buffer swap (which waits for vsync) happens with the lock released.
//
// Layout:
//   header: price, spread, volume, play / speed / reset
//   left:   depth ladder (click a price to use it as your limit) + time & sales
//   center: price chart, depth chart + market settings, market makers
//   right:  order entry with a live slippage estimate, your account, your executions
#include "orderbook/Simulation.hpp"

#include <raylib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace ob;

namespace {

// ---------------------------------------------------------------- look

const Color kBg{14, 17, 22, 255};
const Color kPanel{21, 26, 33, 255};
const Color kPanel2{27, 33, 42, 255};
const Color kLine{38, 46, 57, 255};
const Color kText{214, 221, 230, 255};
const Color kMuted{125, 136, 150, 255};
const Color kBid{38, 194, 129, 255};
const Color kAsk{239, 83, 80, 255};
const Color kFair{90, 169, 230, 255};
const Color kAccent{240, 180, 41, 255};

Color alpha(Color c, float a) { return Color{c.r, c.g, c.b, static_cast<unsigned char>(a * 255)}; }
Color sideColor(Side s) { return s == Side::Buy ? kBid : kAsk; }

Font gSans, gMono;

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char buf[256];
    va_list args;
    va_start(args, f);
    std::vsnprintf(buf, sizeof buf, f, args);
    va_end(args);
    return buf;
}
std::string money(double ticks) { return fmt("%.2f", toDollars(ticks)); }
std::string signedMoney(double dollars) { return fmt("%s$%.2f", dollars < 0 ? "-" : "+", std::abs(dollars)); }
std::string withCommas(long long v) {
    std::string s = std::to_string(std::llabs(v)), out;
    for (int i = 0; i < static_cast<int>(s.size()); ++i) {
        if (i && (s.size() - i) % 3 == 0) out += ',';
        out += s[i];
    }
    return (v < 0 ? "-" : "") + out;
}

void text(const std::string& s, float x, float y, float size, Color c, bool mono = false) {
    DrawTextEx(mono ? gMono : gSans, s.c_str(), {x, y}, size, 0, c);
}
float width(const std::string& s, float size, bool mono = false) { return MeasureTextEx(mono ? gMono : gSans, s.c_str(), size, 0).x; }
void textRight(const std::string& s, float right, float y, float size, Color c, bool mono = false) {
    text(s, right - width(s, size, mono), y, size, c, mono);
}
void textCenter(const std::string& s, Rectangle r, float size, Color c, bool mono = false) {
    text(s, r.x + (r.width - width(s, size, mono)) / 2, r.y + (r.height - size) / 2, size, c, mono);
}
void panel(Rectangle r, const char* title = nullptr) {
    DrawRectangleRec(r, kPanel);
    DrawRectangleLinesEx(r, 1, kLine);
    if (title) text(title, r.x + 12, r.y + 10, 13, kMuted);
}

// ---------------------------------------------------------------- tiny immediate-mode widgets

bool hovered(Rectangle r) { return CheckCollisionPointRec(GetMousePosition(), r); }

bool button(Rectangle r, const std::string& label, Color fill = kPanel2, Color ink = kText, float size = 14) {
    const bool hot = hovered(r);
    DrawRectangleRec(r, hot ? ColorBrightness(fill, 0.12f) : fill);
    DrawRectangleLinesEx(r, 1, hot ? alpha(kText, .35f) : kLine);
    textCenter(label, r, size, ink);
    return hot && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

// Two-way switch; returns true when the value changed.
bool segmented(Rectangle r, const char* a, const char* b, int& value, Color ca, Color cb) {
    const Rectangle left{r.x, r.y, r.width / 2, r.height}, right{r.x + r.width / 2, r.y, r.width / 2, r.height};
    const int before = value;
    if (button(left, a, value == 0 ? ca : kPanel2, value == 0 ? kBg : kMuted, 15)) value = 0;
    if (button(right, b, value == 1 ? cb : kPanel2, value == 1 ? kBg : kMuted, 15)) value = 1;
    return before != value;
}

const void* gDragging = nullptr;
bool slider(Rectangle r, const char* label, float& v, float lo, float hi, const std::string& shown) {
    const float before = v;
    text(label, r.x, r.y, 13, kMuted);
    textRight(shown, r.x + r.width, r.y, 13, kText, true);
    const Rectangle track{r.x, r.y + 20, r.width, 6};
    const Rectangle hit{track.x - 4, track.y - 7, track.width + 8, track.height + 14};
    if (hovered(hit) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) gDragging = &v;
    if (gDragging == &v) {
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) gDragging = nullptr;
        else v = lo + std::clamp((GetMousePosition().x - track.x) / track.width, 0.0f, 1.0f) * (hi - lo);
    }
    const float t = (v - lo) / (hi - lo);
    DrawRectangleRec(track, kPanel2);
    DrawRectangleRec({track.x, track.y, track.width * t, track.height}, alpha(kAccent, .75f));
    DrawCircleV({track.x + track.width * t, track.y + 3}, 7, gDragging == &v ? kText : kAccent);
    return before != v;
}

// ---------------------------------------------------------------- app state

struct UserMark { long step; double price; Side side; };

struct App {
    Simulation sim;
    std::mutex mutex;                    // guards sim (and everything reached through it)
    std::atomic<bool> running{true};
    std::atomic<bool> quit{false};
    std::atomic<int> speedIndex{1};      // see stepsPerSecond()
    std::atomic<double> measuredRate{0}; // steps actually simulated per second

    int side = 0;  // 0 buy, 1 sell
    int type = 0;  // 0 market, 1 limit
    Qty quantity = 300;
    Price limit = 10000;
    bool limitTouched = false;
    std::vector<UserMark> marks;
    std::string flash;
    double flashUntil = 0;

    // knobs mirrored into the sim config
    float volatility, takerRate, informed, makerSpread, makerSize, makerSkew, makerRefresh, makers;

    App() { pullKnobs(); }
    void pullKnobs() {
        const SimConfig& c = sim.config();
        volatility = static_cast<float>(c.volatility);
        takerRate = static_cast<float>(c.takerRate);
        informed = static_cast<float>(c.informedShare);
        makerSpread = static_cast<float>(c.maker.halfSpreadTicks);
        makerSize = static_cast<float>(c.maker.size);
        makerSkew = static_cast<float>(c.maker.skewTicksPer100);
        makerRefresh = static_cast<float>(c.maker.refreshChance);
        makers = static_cast<float>(c.makerCount);
    }
    // 0 means "as fast as the worker thread can go"
    double stepsPerSecond() const { static const double s[] = {4, 12, 40, 120, 0}; return s[speedIndex.load()]; }
    Side orderSide() const { return side == 0 ? Side::Buy : Side::Sell; }
};

// ---------------------------------------------------------------- header

void drawHeader(App& app, Rectangle r) {
    DrawRectangleRec(r, kPanel);
    DrawLine(0, static_cast<int>(r.y + r.height), static_cast<int>(r.width), static_cast<int>(r.y + r.height), kLine);
    const OrderBook& book = app.sim.book();
    text("ORDER BOOK SIM", r.x + 16, r.y + 15, 16, kAccent);
    text("XYZ", r.x + 170, r.y + 9, 13, kMuted);

    const auto& h = app.sim.history();
    const double now = app.sim.mark(), first = h.empty() ? now : h.front().mid;
    const double change = (now - first) / first * 100;
    text(fmt("$%s", money(now).c_str()), r.x + 170, r.y + 24, 18, kText, true);
    text(fmt("%s%.2f%%", change >= 0 ? "+" : "", change), r.x + 270, r.y + 27, 14, change >= 0 ? kBid : kAsk, true);

    float x = r.x + 350;
    auto stat = [&](const char* label, const std::string& value, Color c = kText) {
        text(label, x, r.y + 9, 12, kMuted);
        text(value, x, r.y + 25, 15, c, true);
        x += std::max(96.0f, width(value, 15, true) + 30);
    };
    stat("SPREAD", book.spread() ? fmt("%lld ticks", static_cast<long long>(*book.spread())) : "—");
    stat("FAIR VALUE", "$" + money(app.sim.fairValue()), kFair);
    stat("VOLUME", withCommas(app.sim.volume()));
    stat("RESTING ORDERS", withCommas(static_cast<long long>(book.orderCount())));
    stat("AVG MKT SLIPPAGE", fmt("%.2f bps", app.sim.marketSlippage().averageBps()), kAccent);
    stat("SIM THREAD", app.running ? fmt("%s steps/s", withCommas(std::llround(app.measuredRate.load())).c_str()) : "paused", kMuted);

    float bx = r.x + r.width - 16;
    auto rightButton = [&](const std::string& label, float w, Color fill = kPanel2, Color ink = kText) {
        bx -= w;
        const bool clicked = button({bx, r.y + 12, w, 32}, label, fill, ink, 14);
        bx -= 8;
        return clicked;
    };
    if (rightButton("Reset", 70)) {
        app.sim.reset();
        app.marks.clear();
        app.limitTouched = false;
    }
    if (rightButton("Step", 60)) app.sim.step();
    static const char* speeds[] = {"Slow", "Normal", "Fast", "Turbo", "Max"};
    for (int i = 4; i >= 0; --i)
        if (rightButton(speeds[i], 66, app.speedIndex == i ? alpha(kAccent, .25f) : kPanel2, app.speedIndex == i ? kAccent : kMuted)) app.speedIndex = i;
    if (rightButton(app.running ? "Pause" : "Play", 72, app.running ? kPanel2 : kAccent, app.running ? kText : kBg)) app.running = !app.running.load();
}

// ---------------------------------------------------------------- ladder + tape

void drawLadder(App& app, Rectangle r) {
    panel(r, "ORDER BOOK");
    const OrderBook& book = app.sim.book();
    const int rows = std::max(4, static_cast<int>((r.height - 70) / 2 / 21));
    const auto asks = book.depth(Side::Sell, rows), bids = book.depth(Side::Buy, rows);

    Qty maxQty = 1;
    for (auto& l : asks) maxQty = std::max(maxQty, l.quantity);
    for (auto& l : bids) maxQty = std::max(maxQty, l.quantity);

    std::vector<Price> mine;
    for (const Order& o : book.ordersFor(Simulation::kUser)) mine.push_back(o.price);
    auto isMine = [&](Price p) { return std::find(mine.begin(), mine.end(), p) != mine.end(); };

    const float colPrice = r.x + 92, colSize = r.x + 190, colTotal = r.x + r.width - 60, colOrders = r.x + r.width - 14;
    float y = r.y + 34;
    textRight("PRICE", colPrice, y, 11, kMuted);
    textRight("SIZE", colSize, y, 11, kMuted);
    textRight("TOTAL", colTotal, y, 11, kMuted);
    textRight("ORDERS", colOrders, y, 11, kMuted);
    y += 18;

    auto row = [&](const OrderBook::Level& l, Qty total, Side side, float rowY) {
        const Rectangle rr{r.x + 1, rowY, r.width - 2, 20};
        const float bar = (r.width - 2) * static_cast<float>(l.quantity) / static_cast<float>(maxQty);
        DrawRectangleRec({r.x + r.width - 1 - bar, rowY + 1, bar, 18}, alpha(sideColor(side), .14f));
        if (hovered(rr)) {
            DrawRectangleLinesEx(rr, 1, alpha(kText, .3f));
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                app.type = 1;
                app.limit = l.price;
                app.limitTouched = true;
            }
        }
        if (isMine(l.price)) DrawCircle(static_cast<int>(r.x + 12), static_cast<int>(rowY + 10), 4, kAccent);
        textRight(money(static_cast<double>(l.price)), colPrice, rowY + 2, 15, sideColor(side), true);
        textRight(withCommas(l.quantity), colSize, rowY + 2, 15, kText, true);
        textRight(withCommas(total), colTotal, rowY + 2, 15, kMuted, true);
        textRight(std::to_string(l.orders), colOrders, rowY + 2, 15, kMuted, true);
    };

    // asks: best ask sits just above the spread line, so draw from the top down in reverse
    Qty total = 0;
    std::vector<Qty> askTotals;
    for (auto& l : asks) askTotals.push_back(total += l.quantity);
    for (int i = rows - 1; i >= 0; --i) {
        if (i < static_cast<int>(asks.size())) row(asks[i], askTotals[i], Side::Sell, y);
        y += 21;
    }
    DrawRectangleRec({r.x + 1, y, r.width - 2, 26}, kPanel2);
    const std::string spread = book.spread() ? fmt("spread %lld ticks ($%s)", static_cast<long long>(*book.spread()), money(static_cast<double>(*book.spread())).c_str()) : "one side of the book is empty";
    textCenter(spread, {r.x, y, r.width, 26}, 13, kMuted);
    y += 30;
    total = 0;
    for (auto& l : bids) {
        row(l, total += l.quantity, Side::Buy, y);
        y += 21;
    }
}

void drawTape(App& app, Rectangle r) {
    panel(r, "TIME & SALES");
    float y = r.y + 34;
    const auto& tape = app.sim.book().tape();
    auto nameOf = [&](TraderId id) -> std::string {
        if (id == Simulation::kUser) return "YOU";
        if (id == Simulation::kNoise) return "noise";
        if (id == Simulation::kInformed) return "informed";
        for (auto& m : app.sim.makers()) if (m.id() == id) return m.name();
        return "?";
    };
    for (std::size_t i = 0; i < tape.size() && y < r.y + r.height - 20; ++i) {
        const Fill& f = tape[i];
        const bool mine = f.taker == Simulation::kUser || f.maker == Simulation::kUser;
        text(money(static_cast<double>(f.price)), r.x + 12, y, 14, sideColor(f.takerSide), true);
        textRight(withCommas(f.quantity), r.x + 140, y, 14, kText, true);
        text(fmt("%s hit %s", nameOf(f.taker).c_str(), nameOf(f.maker).c_str()), r.x + 156, y + 1, 12, mine ? kAccent : kMuted);
        y += 19;
    }
}

// ---------------------------------------------------------------- charts

void drawPriceChart(App& app, Rectangle r) {
    panel(r, "PRICE");
    const auto& h = app.sim.history();
    const std::size_t visible = std::min<std::size_t>(h.size(), 360);
    if (visible < 2) return;
    const std::size_t start = h.size() - visible;

    double lo = 1e18, hi = -1e18;
    for (std::size_t i = start; i < h.size(); ++i) {
        lo = std::min({lo, h[i].bid, h[i].fair, h[i].last});
        hi = std::max({hi, h[i].ask, h[i].fair, h[i].last});
    }
    const double pad = std::max(4.0, (hi - lo) * 0.08);
    lo -= pad;
    hi += pad;

    const Rectangle plot{r.x + 12, r.y + 36, r.width - 82, r.height - 50};
    auto X = [&](std::size_t i) { return plot.x + plot.width * static_cast<float>(i - start) / static_cast<float>(visible - 1); };
    auto Y = [&](double p) { return plot.y + plot.height * static_cast<float>((hi - p) / (hi - lo)); };

    for (int g = 0; g <= 4; ++g) {
        const double p = lo + (hi - lo) * g / 4;
        DrawLineEx({plot.x, Y(p)}, {plot.x + plot.width, Y(p)}, 1, alpha(kLine, .7f));
        text("$" + money(p), plot.x + plot.width + 8, Y(p) - 7, 12, kMuted, true);
    }
    for (std::size_t i = start + 1; i < h.size(); ++i) {  // bid/ask band
        const float x0 = X(i - 1), x1 = X(i);
        DrawRectangleRec({x0, Y(h[i].ask), std::max(1.0f, x1 - x0), std::max(1.0f, Y(h[i].bid) - Y(h[i].ask))}, alpha(kText, .07f));
    }
    for (std::size_t i = start + 1; i < h.size(); ++i) {
        if ((i / 3) % 2 == 0) DrawLineEx({X(i - 1), Y(h[i - 1].fair)}, {X(i), Y(h[i].fair)}, 1.5f, alpha(kFair, .8f));
        DrawLineEx({X(i - 1), Y(h[i - 1].mid)}, {X(i), Y(h[i].mid)}, 2, kText);
    }
    for (const UserMark& m : app.marks) {
        if (m.step < h[start].step) continue;
        const std::size_t i = start + static_cast<std::size_t>(m.step - h[start].step);
        if (i >= h.size()) continue;
        const Vector2 c{X(i), Y(m.price)};
        if (m.side == Side::Buy) DrawTriangle({c.x, c.y - 8}, {c.x - 7, c.y + 5}, {c.x + 7, c.y + 5}, kBid);
        else DrawTriangle({c.x - 7, c.y - 5}, {c.x, c.y + 8}, {c.x + 7, c.y - 5}, kAsk);
    }

    float lx = r.x + 90;
    auto legend = [&](const char* label, Color c) {
        DrawRectangleRec({lx, r.y + 15, 14, 3}, c);
        text(label, lx + 20, r.y + 9, 12, kMuted);
        lx += width(label, 12) + 44;
    };
    legend("mid", kText);
    legend("fair value (hidden from traders)", kFair);
    legend("bid-ask spread", alpha(kText, .3f));
    legend("your trades", kAccent);
}

void drawDepthChart(App& app, Rectangle r) {
    panel(r, "DEPTH");
    const OrderBook& book = app.sim.book();
    const auto bids = book.depth(Side::Buy, 40), asks = book.depth(Side::Sell, 40);
    if (bids.empty() || asks.empty()) return;
    const Rectangle plot{r.x + 12, r.y + 34, r.width - 24, r.height - 58};
    const double lo = static_cast<double>(bids.back().price), hi = static_cast<double>(asks.back().price);
    Qty maxCum = 1, cum = 0;
    for (auto& l : bids) cum += l.quantity;
    maxCum = std::max(maxCum, cum);
    cum = 0;
    for (auto& l : asks) cum += l.quantity;
    maxCum = std::max(maxCum, cum);

    auto X = [&](double p) { return plot.x + plot.width * static_cast<float>((p - lo) / std::max(1.0, hi - lo)); };
    auto H = [&](Qty q) { return plot.height * static_cast<float>(q) / static_cast<float>(maxCum); };
    auto area = [&](const std::vector<OrderBook::Level>& levels, Color c, int dir) {
        Qty running = 0;
        for (std::size_t i = 0; i < levels.size(); ++i) {
            running += levels[i].quantity;
            const double next = i + 1 < levels.size() ? static_cast<double>(levels[i + 1].price) : static_cast<double>(levels[i].price) + dir;
            const float x0 = X(static_cast<double>(levels[i].price)), x1 = X(next);
            const float h = H(running);
            DrawRectangleRec({std::min(x0, x1), plot.y + plot.height - h, std::abs(x1 - x0) + 1, h}, alpha(c, .22f));
            DrawLineEx({std::min(x0, x1), plot.y + plot.height - h}, {std::max(x0, x1) + 1, plot.y + plot.height - h}, 2, c);
        }
    };
    area(bids, kBid, -1);
    area(asks, kAsk, 1);
    DrawLineEx({X(*book.mid()), plot.y}, {X(*book.mid()), plot.y + plot.height}, 1, alpha(kText, .4f));
    text("$" + money(lo), plot.x, plot.y + plot.height + 6, 12, kMuted, true);
    textRight("$" + money(hi), plot.x + plot.width, plot.y + plot.height + 6, 12, kMuted, true);
    textRight(withCommas(maxCum) + " shares", plot.x + plot.width, r.y + 10, 12, kMuted, true);
}

// ---------------------------------------------------------------- market settings + makers

void drawSettings(App& app, Rectangle r) {
    panel(r, "MARKET SETTINGS");
    SimConfig& c = app.sim.config();
    const float colW = (r.width - 36) / 2;
    float y = r.y + 36;
    int n = 0;
    auto next = [&]() {
        Rectangle s{r.x + 12 + (n % 2) * (colW + 12), y + (n / 2) * 40.0f, colW, 30};
        ++n;
        return s;
    };
    if (slider(next(), "Volatility", app.volatility, 0, 8, fmt("%.1f ticks", app.volatility))) c.volatility = app.volatility;
    if (slider(next(), "Market orders / step", app.takerRate, 0, 5, fmt("%.1f", app.takerRate))) c.takerRate = app.takerRate;
    if (slider(next(), "Informed traders", app.informed, 0, 1, fmt("%.0f%%", app.informed * 100))) c.informedShare = app.informed;
    if (slider(next(), "Market makers", app.makers, 0, 8, fmt("%d", static_cast<int>(std::lround(app.makers))))) app.sim.setMakerCount(static_cast<int>(std::lround(app.makers)));
    bool makerChange = false;
    makerChange |= slider(next(), "Maker half-spread", app.makerSpread, 0.5f, 12, fmt("%.1f ticks", app.makerSpread));
    makerChange |= slider(next(), "Maker quote size", app.makerSize, 10, 500, fmt("%d", static_cast<int>(app.makerSize)));
    makerChange |= slider(next(), "Inventory skew", app.makerSkew, 0, 6, fmt("%.1f / 100 sh", app.makerSkew));
    makerChange |= slider(next(), "Quote refresh", app.makerRefresh, 0.05f, 1, fmt("%.0f%%", app.makerRefresh * 100));
    if (makerChange) {
        c.maker.halfSpreadTicks = app.makerSpread;
        c.maker.size = static_cast<Qty>(app.makerSize);
        c.maker.skewTicksPer100 = app.makerSkew;
        c.maker.refreshChance = app.makerRefresh;
        auto& makers = app.sim.makers();
        for (std::size_t i = 0; i < makers.size(); ++i) {
            MarketMakerParams& p = makers[i].params();
            p.halfSpreadTicks = app.makerSpread + static_cast<double>(i) * 0.75;
            p.size = static_cast<Qty>(app.makerSize) * static_cast<Qty>(4 + i % 3) / 5;
            p.skewTicksPer100 = app.makerSkew;
            p.refreshChance = app.makerRefresh;
        }
    }
}

void drawMakers(App& app, Rectangle r) {
    panel(r, "MARKET MAKERS");
    const float cols[] = {r.x + 12, r.x + 120, r.x + 270, r.x + 360, r.x + r.width - 170, r.x + r.width - 14};
    float y = r.y + 34;
    text("NAME", cols[0], y, 11, kMuted);
    text("QUOTING (BID × ASK)", cols[1], y, 11, kMuted);
    text("HALF-SPREAD", cols[2], y, 11, kMuted);
    text("INVENTORY", cols[3], y, 11, kMuted);
    textRight("VOLUME", cols[4], y, 11, kMuted);
    textRight("P&L", cols[5], y, 11, kMuted);
    y += 18;
    const double mark = app.sim.mark();
    if (app.sim.makers().empty()) text("No market makers. Watch the spread blow out, then add some back in Market settings.", cols[0], y + 4, 13, kAsk);
    for (auto& m : app.sim.makers()) {
        if (y > r.y + r.height - 22) break;
        const Account& a = app.sim.account(m.id());
        text(m.name(), cols[0], y, 14, kText);
        text(fmt("%s × %s", money(m.lastBid()).c_str(), money(m.lastAsk()).c_str()), cols[1], y, 14, kMuted, true);
        text(fmt("%.1f", m.params().halfSpreadTicks), cols[2], y, 14, kMuted, true);
        const float barW = cols[4] - cols[3] - 120, center = cols[3] + barW / 2;
        DrawRectangleRec({cols[3], y + 6, barW, 6}, kPanel2);
        const float fill = std::clamp(static_cast<float>(a.position) / static_cast<float>(m.params().maxInventory), -1.0f, 1.0f) * barW / 2;
        DrawRectangleRec({fill < 0 ? center + fill : center, y + 6, std::abs(fill), 6}, a.position < 0 ? kAsk : kBid);
        DrawLineEx({center, y + 3}, {center, y + 15}, 1, kMuted);
        text(withCommas(a.position), cols[3] + barW + 10, y, 14, kText, true);
        textRight(withCommas(a.volume), cols[4], y, 14, kMuted, true);
        const double pnl = a.pnl(mark);
        textRight(signedMoney(pnl), cols[5], y, 14, pnl >= 0 ? kBid : kAsk, true);
        y += 22;
    }
}

// ---------------------------------------------------------------- order entry + account

void submit(App& app) {
    const Price limit = app.type == 1 ? app.limit : 0;
    const ExecutionReport r = app.sim.submitUser(app.orderSide(), app.type == 1 ? OrderType::Limit : OrderType::Market, app.quantity, limit);
    if (r.filled > 0) app.marks.push_back({app.sim.steps(), r.averagePrice, r.side});
    if (r.filled > 0)
        app.flash = fmt("%s %s @ $%s  ·  slippage %.2f bps", r.side == Side::Buy ? "Bought" : "Sold", withCommas(r.filled).c_str(), money(r.averagePrice).c_str(), r.slippageBps());
    else if (r.rested > 0)
        app.flash = fmt("Limit order resting: %s @ $%s", withCommas(r.rested).c_str(), money(static_cast<double>(r.limit)).c_str());
    else
        app.flash = "Nothing filled: the book had no liquidity on that side.";
    if (r.unfilled > 0) app.flash += fmt("  ·  %s unfilled", withCommas(r.unfilled).c_str());
    app.flashUntil = GetTime() + 5;
}

void drawOrderEntry(App& app, Rectangle r) {
    panel(r, "NEW ORDER");
    const OrderBook& book = app.sim.book();
    float x = r.x + 14, w = r.width - 28, y = r.y + 34;

    segmented({x, y, w, 38}, "BUY", "SELL", app.side, kBid, kAsk);
    y += 46;
    if (segmented({x, y, w, 30}, "MARKET", "LIMIT", app.type, kText, kText) && app.type == 1 && !app.limitTouched) {
        const auto touch = app.orderSide() == Side::Buy ? book.bestBid() : book.bestAsk();
        app.limit = touch ? *touch : static_cast<Price>(app.sim.mark());
    }
    y += 42;

    // quantity
    text("QUANTITY", x, y, 11, kMuted);
    y += 16;
    const float bw = 44;
    if (button({x, y, bw, 32}, "-100")) app.quantity = std::max<Qty>(10, app.quantity - 100);
    if (button({x + bw + 4, y, bw, 32}, "-10")) app.quantity = std::max<Qty>(10, app.quantity - 10);
    DrawRectangleRec({x + 2 * (bw + 4), y, w - 4 * (bw + 4), 32}, kBg);
    textCenter(withCommas(app.quantity), {x + 2 * (bw + 4), y, w - 4 * (bw + 4), 32}, 18, kText, true);
    if (button({x + w - 2 * bw - 4, y, bw, 32}, "+10")) app.quantity += 10;
    if (button({x + w - bw, y, bw, 32}, "+100")) app.quantity += 100;
    y += 42;

    // limit price
    if (app.type == 1) {
        text("LIMIT PRICE  ·  or click a row in the book", x, y, 11, kMuted);
        y += 16;
        if (button({x, y, bw, 32}, "-10")) { app.limit -= 10; app.limitTouched = true; }
        if (button({x + bw + 4, y, bw, 32}, "-1")) { app.limit -= 1; app.limitTouched = true; }
        DrawRectangleRec({x + 2 * (bw + 4), y, w - 4 * (bw + 4), 32}, kBg);
        textCenter("$" + money(static_cast<double>(app.limit)), {x + 2 * (bw + 4), y, w - 4 * (bw + 4), 32}, 18, kText, true);
        if (button({x + w - 2 * bw - 4, y, bw, 32}, "+1")) { app.limit += 1; app.limitTouched = true; }
        if (button({x + w - bw, y, bw, 32}, "+10")) { app.limit += 10; app.limitTouched = true; }
        y += 38;
        const float hw = (w - 8) / 3;
        auto bestSame = app.orderSide() == Side::Buy ? book.bestBid() : book.bestAsk();
        auto bestOpp = app.orderSide() == Side::Buy ? book.bestAsk() : book.bestBid();
        if (button({x, y, hw, 26}, "Join best", kPanel2, kMuted, 12) && bestSame) { app.limit = *bestSame; app.limitTouched = true; }
        if (button({x + hw + 4, y, hw, 26}, "Mid", kPanel2, kMuted, 12)) { app.limit = static_cast<Price>(std::round(app.sim.mark())); app.limitTouched = true; }
        if (button({x + 2 * (hw + 4), y, hw, 26}, "Cross spread", kPanel2, kMuted, 12) && bestOpp) { app.limit = *bestOpp; app.limitTouched = true; }
        y += 34;
    }

    // pre-trade estimate: walk the book as if this were a market order (capped at the limit for limits)
    DrawRectangleRec({x, y, w, 104}, kBg);
    const ExecutionReport est = book.estimateMarket(app.orderSide(), app.quantity);
    const bool marketable = app.type == 0 || (est.arrivalTouch && direction(app.orderSide()) * (app.limit - est.arrivalTouch) >= 0);
    float ey = y + 10;
    auto line = [&](const char* label, const std::string& value, Color c = kText) {
        text(label, x + 12, ey, 13, kMuted);
        textRight(value, x + w - 12, ey, 14, c, true);
        ey += 22;
    };
    if (!marketable) {
        text("PASSIVE ORDER", x + 12, ey, 11, kAccent);
        ey += 20;
        line("Rests in the book at", "$" + money(static_cast<double>(app.limit)));
        line("No slippage now. You wait to be filled", "");
        line("and earn the spread instead of paying it.", "");
    } else {
        text(app.type == 0 ? "IF YOU SEND THIS NOW" : "CROSSES THE SPREAD: FILLS NOW UP TO YOUR LIMIT", x + 12, ey, 11, kAccent);
        ey += 20;
        if (est.filled == 0) {
            line("Fills", "nothing (empty book)", kAsk);
        } else {
            line("Est. average price", "$" + money(est.averagePrice));
            line("Slippage vs mid", fmt("%.2f ticks · %.2f bps", est.slippageVsMidTicks(), est.slippageBps()), est.slippageBps() > 5 ? kAsk : kAccent);
            line("Cost · levels swept", fmt("$%.2f · %d%s", est.slippageCost(), est.levelsSwept, est.unfilled ? fmt(" · %lld unfilled", static_cast<long long>(est.unfilled)).c_str() : ""), est.unfilled ? kAsk : kText);
        }
    }
    y += 114;

    const std::string label = fmt("%s %s %s", app.side == 0 ? "BUY" : "SELL", withCommas(app.quantity).c_str(),
                                  app.type == 0 ? "AT MARKET" : ("AT $" + money(static_cast<double>(app.limit))).c_str());
    if (button({x, y, w, 42}, label, app.side == 0 ? kBid : kAsk, kBg, 16) || IsKeyPressed(KEY_ENTER)) submit(app);
    y += 50;
    if (GetTime() < app.flashUntil) text(app.flash, x, y, 12, kAccent);
}

void drawAccount(App& app, Rectangle r) {
    panel(r, "YOUR ACCOUNT");
    const Account& a = app.sim.account(Simulation::kUser);
    const double pnl = a.pnl(app.sim.mark());
    float x = r.x + 14, y = r.y + 34;
    const float third = (r.width - 28) / 3;
    auto box = [&](float bx, const char* label, const std::string& value, Color c) {
        text(label, bx, y, 11, kMuted);
        text(value, bx, y + 15, 17, c, true);
    };
    box(x, "POSITION", withCommas(a.position), kText);
    box(x + third, "CASH", signedMoney(a.cash), kMuted);
    box(x + 2 * third, "P&L (AT MID)", signedMoney(pnl), pnl >= 0 ? kBid : kAsk);
    y += 46;

    const auto open = app.sim.book().ordersFor(Simulation::kUser);
    text(fmt("OPEN ORDERS (%zu)", open.size()), x, y, 11, kMuted);
    if (!open.empty() && button({r.x + r.width - 104, y - 4, 90, 20}, "Cancel all", kPanel2, kMuted, 11)) app.sim.book().cancelAll(Simulation::kUser);
    y += 20;
    for (std::size_t i = 0; i < open.size() && i < 3; ++i) {
        const Order& o = open[i];
        text(fmt("%s %s @ $%s", toString(o.side), withCommas(o.remaining).c_str(), money(static_cast<double>(o.price)).c_str()), x, y, 13, sideColor(o.side), true);
        if (button({r.x + r.width - 40, y - 2, 26, 20}, "×", kPanel2, kMuted, 13)) app.sim.book().cancel(o.id);
        y += 22;
    }
    if (open.empty()) { text("None", x, y, 13, kMuted); y += 22; }
    y += 6;

    text("YOUR EXECUTIONS · SLIPPAGE VS ARRIVAL MID", x, y, 11, kMuted);
    y += 20;
    for (const ExecutionReport& e : app.sim.userReports()) {
        if (y > r.y + r.height - 20) break;
        if (e.filled == 0) continue;
        text(fmt("%s %s @ $%s", toString(e.side), withCommas(e.filled).c_str(), money(e.averagePrice).c_str()), x, y, 13, sideColor(e.side), true);
        textRight(fmt("%.2f bps · %d lvl", e.slippageBps(), e.levelsSwept), r.x + r.width - 14, y, 13, e.slippageBps() > 5 ? kAsk : kMuted, true);
        y += 20;
    }
}

// Worker thread: advances the market at the chosen speed, in small locked batches so the
// GUI thread can always get in between them.
void simLoop(App& app) {
    using Clock = std::chrono::steady_clock;
    auto last = Clock::now(), windowStart = last;
    double owed = 0;
    long counted = 0;
    while (!app.quit) {
        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (!app.running) {
            owed = 0;
            app.measuredRate = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const double rate = app.stepsPerSecond();
        int batch;
        if (rate > 0) {
            owed = std::min(owed + dt * rate, 200.0);
            batch = static_cast<int>(owed);
            owed -= batch;
        } else {
            batch = 200;  // Max: as many as fit in a short slice
        }
        if (batch > 0) {
            std::lock_guard<std::mutex> lock(app.mutex);
            for (int i = 0; i < batch; ++i) app.sim.step();
        }
        counted += batch;
        const double window = std::chrono::duration<double>(Clock::now() - windowStart).count();
        if (window >= 0.5) {
            app.measuredRate = counted / window;
            counted = 0;
            windowStart = Clock::now();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(rate > 0 ? 2000 : 300));  // let the GUI take the lock
    }
}

}  // namespace

int main(int argc, char** argv) {
    // --screenshot <file>: render one frame after a short warm-up, save it, and exit (used for the README).
    const char* screenshotPath = nullptr;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--screenshot") screenshotPath = argv[i + 1];

    // High-DPI rendering is skipped for --screenshot: raylib saves high-DPI screenshots at the wrong scale.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | (screenshotPath ? 0 : FLAG_WINDOW_HIGHDPI));
    InitWindow(1500, 940, "Order Book Sim");
    SetWindowMinSize(1280, 820);
    SetTargetFPS(60);

    // Load fonts with the few non-ASCII symbols the UI uses.
    std::vector<int> glyphs;
    for (int c = 32; c < 127; ++c) glyphs.push_back(c);
    for (int c : {0x00B7, 0x00D7, 0x2014, 0x2013, 0x2022}) glyphs.push_back(c);
    auto load = [&](std::initializer_list<const char*> paths) {
        for (const char* p : paths)
            if (FileExists(p)) {
                Font f = LoadFontEx(p, 48, glyphs.data(), static_cast<int>(glyphs.size()));
                SetTextureFilter(f.texture, TEXTURE_FILTER_BILINEAR);
                return f;
            }
        return GetFontDefault();
    };
    gSans = load({"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf"});
    gMono = load({"/System/Library/Fonts/SFNSMono.ttf", "/System/Library/Fonts/Supplemental/Andale Mono.ttf"});

    App app;
    if (screenshotPath) {
        app.running = false;
        for (int i = 0; i < 400; ++i) app.sim.step();
        app.sim.submitUser(Side::Buy, OrderType::Market, 900);
        for (int i = 0; i < 60; ++i) app.sim.step();
        app.sim.submitUser(Side::Sell, OrderType::Limit, 300, static_cast<Price>(std::round(app.sim.mark())) + 6);
        for (int i = 0; i < 40; ++i) app.sim.step();
    }
    std::thread worker(simLoop, std::ref(app));
    int frames = 0;
    while (!WindowShouldClose()) {
        if (IsKeyPressed(KEY_SPACE)) app.running = !app.running.load();

        const float W = static_cast<float>(GetScreenWidth()), H = static_cast<float>(GetScreenHeight());
        const float headerH = 56, gap = 8, left = 360, right = 370;
        const float top = headerH + gap, colH = H - top - gap;
        const float cx = left + 2 * gap, cw = W - left - right - 4 * gap;

        BeginDrawing();
        std::unique_lock<std::mutex> lock(app.mutex);  // read the market and queue draw calls
        ClearBackground(kBg);
        drawHeader(app, {0, 0, W, headerH});

        const float ladderH = colH * 0.66f;
        drawLadder(app, {gap, top, left, ladderH});
        drawTape(app, {gap, top + ladderH + gap, left, colH - ladderH - gap});

        const float chartH = colH * 0.42f, midH = colH * 0.30f, makersH = colH - chartH - midH - 2 * gap;
        drawPriceChart(app, {cx, top, cw, chartH});
        const float depthW = cw * 0.42f;
        drawDepthChart(app, {cx, top + chartH + gap, depthW, midH});
        drawSettings(app, {cx + depthW + gap, top + chartH + gap, cw - depthW - gap, midH});
        drawMakers(app, {cx, top + chartH + midH + 2 * gap, cw, makersH});

        const float rx = W - right - gap, entryH = app.type == 1 ? 470 : 390;
        drawOrderEntry(app, {rx, top, right, entryH});
        drawAccount(app, {rx, top + entryH + gap, right, colH - entryH - gap});
        lock.unlock();  // the swap below waits for vsync; don't hold the sim up for it
        EndDrawing();
        if (screenshotPath && ++frames == 3) {
            TakeScreenshot(screenshotPath);
            break;
        }
    }
    app.quit = true;
    worker.join();
    UnloadFont(gSans);
    UnloadFont(gMono);
    CloseWindow();
    return 0;
}
