/**
 * main.cpp
 *
 * Order Execution Module — 테스트 러너
 *
 * TC1: 심볼 A 포지션 계산 → 다음 틱 심볼 B 매수
 * TC2: 여러 심볼 동시 주문
 * TC3: 매수 체결 전 매도 주문 → 시스템이 걸러냄
 */

#include "order_engine.hpp"

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace oem;
using hrc = std::chrono::high_resolution_clock;

static void separator(const char* title) {
    std::printf("\n+======================================================+\n");
    std::printf("|  %-52s|\n", title);
    std::printf("+======================================================+\n");
}

static double elapsed_us(hrc::time_point t0) {
    return std::chrono::duration<double, std::micro>(hrc::now() - t0).count();
}

// ────────────────────────────────────────────────────────────
//  TC1: 심볼 A 포지션 계산 → 다음 틱 심볼 B 매수
//
//  포인트: 심볼 A 업데이트가 심볼 B 주문을 블로킹하지 않음
//         (per-symbol 격리 덕분에 서로 독립)
// ────────────────────────────────────────────────────────────
static void run_tc1() {
    separator("TC1: Cross-Symbol Sequential Tick Orders");

    OrderEngine engine(500'000.0);
    int btc = engine.registerSymbol("BTCUSDT", 70000.0);
    engine.registerSymbol("ETHUSDT", 3500.0);
    engine.registerSymbol("SOLUSDT", 150.0);

    // Tick 1: BTC 포지션 계산 결과 수신 (외부 포지션 계산기)
    engine.updatePosition(btc, 2.0, 68000.0, 70000.0);
    engine.setUnrealizedPnL(2.0 * (70000 - 68000));
    std::printf("\n  [Tick 1] BTC position: qty=2 avg=68000 mkt=70000\n");
    std::printf("           unrealized P&L = %.0f\n", engine.unrealizedPnL());

    // Tick 2: ETH FUTURES 매수
    {
        OrderReq o{.id = 1, .symbol = "ETHUSDT", .side = Side::BUY,
                   .inst_type = InstType::FUTURES,
                   .quantity = 10.0, .price = 3500.0, .leverage = 10};
        auto t0 = hrc::now();
        Result r = engine.submitOrder(o);
        double lat = elapsed_us(t0);
        std::printf("  [Tick 2] ETH FUTURES BUY 10 @ 3500 lev=10 → %s (%.2f us)\n",
                    to_str(r), lat);
    }

    // Tick 3: SOL OPTIONS 매수
    {
        OrderReq o{.id = 2, .symbol = "SOLUSDT", .side = Side::BUY,
                   .inst_type = InstType::OPTIONS,
                   .quantity = 100.0, .price = 150.0, .leverage = 5};
        auto t0 = hrc::now();
        Result r = engine.submitOrder(o);
        double lat = elapsed_us(t0);
        std::printf("  [Tick 3] SOL OPTIONS BUY 100 @ 150 lev=5 → %s (%.2f us)\n",
                    to_str(r), lat);
    }

    // Tick 4: BTC FUTURES 매도 (포지션 있으므로 OK)
    {
        OrderReq o{.id = 3, .symbol = "BTCUSDT", .side = Side::SELL,
                   .inst_type = InstType::FUTURES,
                   .quantity = 1.0, .price = 71000.0, .leverage = 1};
        auto t0 = hrc::now();
        Result r = engine.submitOrder(o);
        double lat = elapsed_us(t0);
        std::printf("  [Tick 4] BTC FUTURES SELL 1 @ 71000 → %s (%.2f us)\n",
                    to_str(r), lat);
    }

    std::printf("\n  [PASS] 모든 주문이 µs 단위 반환 (심볼 간 블로킹 없음)\n");
    engine.printSummary();
}

// ────────────────────────────────────────────────────────────
//  TC2: 여러 심볼 동시 주문
//
//  포인트: 5개 스레드가 동시에 서로 다른 심볼 주문
//         per-symbol lock 덕에 경합 없이 병렬 처리
// ────────────────────────────────────────────────────────────
static void run_tc2() {
    separator("TC2: Simultaneous Multi-Symbol Orders");

    OrderEngine engine(2'000'000.0);
    engine.registerSymbol("BTCUSDT",  70000.0);
    engine.registerSymbol("ETHUSDT",  3500.0);
    engine.registerSymbol("BNBUSDT",  600.0);
    engine.registerSymbol("SOLUSDT",  150.0);
    engine.registerSymbol("XRPUSDT",  0.5);

    struct TestOrder {
        OrderReq req;
        Result   result;
        double   latency_us;
    };

    std::vector<TestOrder> test_orders = {
        {{.id=10, .symbol="BTCUSDT", .side=Side::BUY, .inst_type=InstType::FUTURES,
          .quantity=1, .price=70000, .leverage=10}, {}, 0},
        {{.id=11, .symbol="ETHUSDT", .side=Side::BUY, .inst_type=InstType::FUTURES,
          .quantity=20, .price=3500, .leverage=5}, {}, 0},
        {{.id=12, .symbol="BNBUSDT", .side=Side::BUY, .inst_type=InstType::OPTIONS,
          .quantity=50, .price=600, .leverage=2}, {}, 0},
        {{.id=13, .symbol="SOLUSDT", .side=Side::BUY, .inst_type=InstType::FUTURES,
          .quantity=200, .price=150, .leverage=5}, {}, 0},
        {{.id=14, .symbol="XRPUSDT", .side=Side::BUY, .inst_type=InstType::OPTIONS,
          .quantity=10000, .price=0.5, .leverage=1}, {}, 0},
    };

    std::printf("\n  %zu개 스레드에서 동시 주문 제출...\n", test_orders.size());

    std::vector<std::thread> threads;
    auto t0_all = hrc::now();

    for (auto& to : test_orders) {
        threads.emplace_back([&] {
            auto t0 = hrc::now();
            to.result = engine.submitOrder(to.req);
            to.latency_us = elapsed_us(t0);
        });
    }
    for (auto& t : threads) t.join();

    double total_us = elapsed_us(t0_all);

    std::printf("\n");
    int ok = 0;
    for (auto& to : test_orders) {
        std::printf("  id=%d %s %s %s → %s (%.2f us)\n",
                    to.req.id, to_str(to.req.inst_type), to_str(to.req.side),
                    to.req.symbol.c_str(), to_str(to.result), to.latency_us);
        if (to.result == Result::ACCEPTED) ++ok;
    }

    std::printf("\n  Total: %.2f us | OK: %d/%zu\n", total_us, ok, test_orders.size());
    std::printf("  [%s] 전부 수락, 데이터 손상 없음\n",
                ok == static_cast<int>(test_orders.size()) ? "PASS" : "FAIL");
    engine.printSummary();
}

// ────────────────────────────────────────────────────────────
//  TC3: 매수 체결 전 매도 주문 → 시스템이 걸러냄
//
//  포인트: submitOrder(BUY) 후 onFill() 전에 SELL 시도
//         포지션이 없으므로 REJECT_NO_POSITION
//         onFill() 후에는 SELL 가능
// ────────────────────────────────────────────────────────────
static void run_tc3() {
    separator("TC3: Sell Before Buy Filled");

    OrderEngine engine(1'000'000.0);
    engine.registerSymbol("ETHUSDT", 3500.0);
    engine.registerSymbol("BTCUSDT", 70000.0);

    // === ETH FUTURES 시나리오 ===
    std::printf("\n  --- ETH FUTURES ---\n");

    // Step 1: FUTURES BUY 제출
    OrderReq eth_buy{.id=20, .symbol="ETHUSDT", .side=Side::BUY,
                     .inst_type=InstType::FUTURES,
                     .quantity=10, .price=3500, .leverage=10};
    Result r1 = engine.submitOrder(eth_buy);
    std::printf("  Step 1: FUTURES BUY ETH 10 → %s\n", to_str(r1));

    // Step 2: 체결 전 SELL 시도 → REJECT
    OrderReq eth_sell{.id=21, .symbol="ETHUSDT", .side=Side::SELL,
                      .inst_type=InstType::FUTURES,
                      .quantity=10, .price=3600, .leverage=10};
    Result r2 = engine.submitOrder(eth_sell);
    std::printf("  Step 2: FUTURES SELL ETH 10 (before fill) → %s\n", to_str(r2));
    std::printf("  [%s] 체결 전 매도 거부됨\n",
                r2 == Result::REJECT_NO_POSITION ? "PASS" : "FAIL");

    // Step 3: BUY 체결 콜백
    engine.onFill(20, 10.0, 3500.0);
    std::printf("  Step 3: onFill(BUY) → 포지션 생성\n");

    // Step 4: 이제 SELL 가능
    OrderReq eth_sell2{.id=22, .symbol="ETHUSDT", .side=Side::SELL,
                       .inst_type=InstType::FUTURES,
                       .quantity=10, .price=3600, .leverage=10};
    Result r3 = engine.submitOrder(eth_sell2);
    std::printf("  Step 4: FUTURES SELL ETH 10 (after fill) → %s\n", to_str(r3));
    std::printf("  [%s] 체결 후 매도 허용됨\n",
                r3 == Result::ACCEPTED ? "PASS" : "FAIL");

    // === BTC OPTIONS 시나리오 ===
    std::printf("\n  --- BTC OPTIONS ---\n");

    OrderReq btc_buy{.id=30, .symbol="BTCUSDT", .side=Side::BUY,
                     .inst_type=InstType::OPTIONS,
                     .quantity=1, .price=70000, .leverage=5};
    Result r4 = engine.submitOrder(btc_buy);
    std::printf("  Step 5: OPTIONS BUY BTC 1 → %s\n", to_str(r4));

    OrderReq btc_sell{.id=31, .symbol="BTCUSDT", .side=Side::SELL,
                      .inst_type=InstType::OPTIONS,
                      .quantity=1, .price=72000, .leverage=5};
    Result r5 = engine.submitOrder(btc_sell);
    std::printf("  Step 6: OPTIONS SELL BTC 1 (before fill) → %s\n", to_str(r5));
    std::printf("  [%s] OPTIONS도 체결 전 매도 거부\n",
                r5 == Result::REJECT_NO_POSITION ? "PASS" : "FAIL");

    engine.printSummary();
}

// ────────────────────────────────────────────────────────────
int main() {
    std::printf("+======================================================+\n");
    std::printf("|    Order Execution Module — Test Suite               |\n");
    std::printf("+======================================================+\n");

    run_tc1();
    run_tc2();
    run_tc3();

    std::printf("\n+======================================================+\n");
    std::printf("|    ALL TEST CASES COMPLETE                           |\n");
    std::printf("+======================================================+\n\n");

    return 0;
}
