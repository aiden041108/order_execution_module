/**
 * dummy_runner.cpp
 *
 * CSV 더미 데이터를 로드해서 OrderEngine으로 실험하는 러너.
 * dummy_data/ 폴더의 CSV 파일을 읽어 3가지 TC를 자동 실행.
 */

#include "order_engine.hpp"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace oem;
using hrc = std::chrono::high_resolution_clock;

// ── CSV 파서 ──

struct CsvOrder {
    int         id;
    std::string symbol;
    Side        side;
    InstType    inst_type;
    double      quantity;
    double      price;
    int         leverage;
    std::string action;  // TC3용: submit, submit_before_fill, fill_order_*, submit_after_fill
};

static std::vector<CsvOrder> load_csv(const std::string& path) {
    std::vector<CsvOrder> out;
    std::ifstream f(path);
    if (!f.is_open()) {
        // 빌드 디렉토리에서 실행 시 ../dummy_data 경로 시도
        f.open("../" + path);
        if (!f.is_open()) {
            std::printf("  [WARN] %s 파일을 찾을 수 없음\n", path.c_str());
            return out;
        }
    }

    std::string line;
    std::getline(f, line);  // 헤더 스킵

    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tok;
        CsvOrder o{};

        if (!std::getline(ss, tok, ',')) continue;
        o.id = std::stoi(tok);

        if (!std::getline(ss, o.symbol, ',')) continue;

        if (!std::getline(ss, tok, ',')) continue;
        o.side = (tok == "SELL") ? Side::SELL : Side::BUY;

        if (!std::getline(ss, tok, ',')) continue;
        o.inst_type = (tok == "OPTIONS") ? InstType::OPTIONS : InstType::FUTURES;

        if (!std::getline(ss, tok, ',')) continue;
        o.quantity = std::stod(tok);

        if (!std::getline(ss, tok, ',')) continue;
        o.price = std::stod(tok);

        if (!std::getline(ss, tok, ',')) continue;
        o.leverage = std::stoi(tok);

        // action 컬럼 (TC3 전용, 없으면 빈 문자열)
        if (std::getline(ss, tok, ','))
            o.action = tok;

        out.push_back(std::move(o));
    }
    return out;
}

static OrderReq to_order_req(const CsvOrder& co) {
    OrderReq o;
    o.id        = co.id;
    o.symbol    = co.symbol;
    o.side      = co.side;
    o.inst_type = co.inst_type;
    o.quantity  = co.quantity;
    o.price     = co.price;
    o.leverage  = co.leverage;
    return o;
}

static void separator(const char* title) {
    std::printf("\n+======================================================+\n");
    std::printf("|  %-52s|\n", title);
    std::printf("+======================================================+\n");
}

static double elapsed_us(hrc::time_point t0) {
    return std::chrono::duration<double, std::micro>(hrc::now() - t0).count();
}

// ── TC1: 순차 주문 ──
static void run_tc1_from_csv() {
    separator("TC1 (CSV): Sequential Cross-Symbol Orders");

    auto rows = load_csv("dummy_data/tc1_sequential.csv");
    if (rows.empty()) return;

    OrderEngine engine(500'000.0);

    // 초기 BTC 포지션 설정 (SELL 주문 있으므로)
    int btc = engine.registerSymbol("BTCUSDT", 69500.0);
    engine.updatePosition(btc, 2.0, 65000.0, 69500.0);
    engine.setUnrealizedPnL(2.0 * (69500 - 65000));
    std::printf("\n  초기 BTC 포지션: qty=2 avg=65000 mkt=69500\n\n");

    for (auto& row : rows) {
        engine.registerSymbol(row.symbol, row.price);
    }

    std::printf("  %zu개 주문 순차 제출:\n", rows.size());
    for (auto& row : rows) {
        auto req = to_order_req(row);
        auto t0 = hrc::now();
        Result r = engine.submitOrder(req);
        double lat = elapsed_us(t0);
        std::printf("    id=%d %s %s %s qty=%.1f px=%.0f → %s (%.2f us)\n",
                    row.id, to_str(row.inst_type), to_str(row.side),
                    row.symbol.c_str(), row.quantity, row.price,
                    to_str(r), lat);
    }

    engine.printSummary();
}

// ── TC2: 동시 주문 ──
static void run_tc2_from_csv() {
    separator("TC2 (CSV): Simultaneous Multi-Symbol Orders");

    auto rows = load_csv("dummy_data/tc2_simultaneous.csv");
    if (rows.empty()) return;

    OrderEngine engine(5'000'000.0);

    for (auto& row : rows)
        engine.registerSymbol(row.symbol, row.price);

    std::printf("\n  %zu개 주문을 %zu개 스레드에서 동시 제출:\n",
                rows.size(), rows.size());

    std::vector<Result> results(rows.size());
    std::vector<double> latencies(rows.size());
    std::vector<std::thread> threads;

    auto t0_all = hrc::now();
    for (size_t i = 0; i < rows.size(); ++i) {
        threads.emplace_back([&, i] {
            auto req = to_order_req(rows[i]);
            auto t0 = hrc::now();
            results[i] = engine.submitOrder(req);
            latencies[i] = elapsed_us(t0);
        });
    }
    for (auto& t : threads) t.join();
    double total_us = elapsed_us(t0_all);

    std::printf("\n");
    int ok = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        std::printf("    id=%d %s %s → %s (%.2f us)\n",
                    rows[i].id, rows[i].symbol.c_str(),
                    to_str(rows[i].side), to_str(results[i]), latencies[i]);
        if (results[i] == Result::ACCEPTED) ++ok;
    }

    std::printf("\n  Total: %.2f us | OK: %d/%zu\n", total_us, ok, rows.size());
    std::printf("  [%s]\n", ok == static_cast<int>(rows.size()) ? "PASS" : "PARTIAL");
    engine.printSummary();
}

// ── TC3: 매수 체결 전 매도 ──
static void run_tc3_from_csv() {
    separator("TC3 (CSV): Sell Before Buy Filled");

    auto rows = load_csv("dummy_data/tc3_sell_before_fill.csv");
    if (rows.empty()) return;

    OrderEngine engine(2'000'000.0);
    for (auto& row : rows)
        engine.registerSymbol(row.symbol, row.price);

    std::printf("\n");
    int pass = 0, total_checks = 0;

    for (auto& row : rows) {
        if (row.action.empty() || row.action == "submit") {
            auto req = to_order_req(row);
            Result r = engine.submitOrder(req);
            std::printf("  [submit]  id=%d %s %s %s → %s\n",
                        row.id, to_str(row.inst_type), to_str(row.side),
                        row.symbol.c_str(), to_str(r));
        }
        else if (row.action == "submit_before_fill") {
            auto req = to_order_req(row);
            Result r = engine.submitOrder(req);
            bool expected_reject = (r == Result::REJECT_NO_POSITION);
            std::printf("  [before_fill] id=%d %s %s → %s [%s]\n",
                        row.id, to_str(row.side), row.symbol.c_str(),
                        to_str(r), expected_reject ? "PASS" : "FAIL");
            if (expected_reject) ++pass;
            ++total_checks;
        }
        else if (row.action.substr(0, 10) == "fill_order") {
            // "fill_order_201" → fill order 201
            int fill_id = std::stoi(row.action.substr(11));
            engine.onFill(fill_id, row.quantity, row.price);
            std::printf("  [fill]    id=%d → onFill(qty=%.1f px=%.0f)\n",
                        fill_id, row.quantity, row.price);
        }
        else if (row.action == "submit_after_fill") {
            auto req = to_order_req(row);
            Result r = engine.submitOrder(req);
            bool expected_accept = (r == Result::ACCEPTED);
            std::printf("  [after_fill]  id=%d %s %s → %s [%s]\n",
                        row.id, to_str(row.side), row.symbol.c_str(),
                        to_str(r), expected_accept ? "PASS" : "FAIL");
            if (expected_accept) ++pass;
            ++total_checks;
        }
    }

    std::printf("\n  검증: %d/%d PASS\n", pass, total_checks);
    engine.printSummary();
}

// ── Main ──
int main() {
    std::printf("+======================================================+\n");
    std::printf("|   Dummy Data Runner — CSV 기반 실험                   |\n");
    std::printf("+======================================================+\n");

    run_tc1_from_csv();
    run_tc2_from_csv();
    run_tc3_from_csv();

    std::printf("\n+======================================================+\n");
    std::printf("|   ALL DUMMY DATA TESTS COMPLETE                      |\n");
    std::printf("+======================================================+\n\n");

    return 0;
}
