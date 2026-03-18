/**
 * order_engine.hpp
 *
 * 저지연 주문 실행 엔진
 *
 * 설계 원칙:
 *   1. Atomic CAS 현금 관리 — 현금 검사+차감을 lock 없이 원자적 수행
 *   2. Per-symbol 격리 — 심볼별 독립 mutex, alignas(64)로 false sharing 방지
 *   3. 인라인 실행 — 큐/스레드풀 없이 호출자 스레드에서 즉시 처리
 *   4. 심볼 레지스트리 — 문자열→정수 매핑, 배열 인덱싱 O(1)
 *
 * C++20 required
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

namespace oem {

// ─── 열거형 ───

enum class Side     : uint8_t { BUY, SELL };
enum class InstType : uint8_t { FUTURES, OPTIONS };

enum class Result : uint8_t {
    ACCEPTED,              // 주문 수락 → 거래소 전송됨
    REJECT_CASH,           // 잔고 부족
    REJECT_NO_POSITION,    // 매도할 포지션 없음
};

inline const char* to_str(Result r) {
    switch (r) {
        case Result::ACCEPTED:          return "ACCEPTED";
        case Result::REJECT_CASH:       return "REJECT_CASH";
        case Result::REJECT_NO_POSITION: return "REJECT_NO_POSITION";
    }
    return "?";
}
inline const char* to_str(Side s)     { return s == Side::BUY ? "BUY" : "SELL"; }
inline const char* to_str(InstType t) { return t == InstType::FUTURES ? "FUTURES" : "OPTIONS"; }

// ─── 주문 요청 ───

struct OrderReq {
    int         id        = 0;
    std::string symbol;
    Side        side      = Side::BUY;
    InstType    inst_type = InstType::FUTURES;
    double      quantity  = 0.0;
    double      price     = 0.0;
    int         leverage  = 1;

    double notional() const { return price * quantity; }
    double margin()   const { return notional() / (leverage > 0 ? leverage : 1); }
};

// ─── 포지션 ───

struct Position {
    double net_qty      = 0.0;
    double avg_price    = 0.0;
    double market_price = 0.0;

    double unrealized_pnl() const { return net_qty * (market_price - avg_price); }
    bool   has_position()   const { return std::abs(net_qty) > 1e-9; }
};

// ─── OrderEngine ───

class OrderEngine {
public:
    explicit OrderEngine(double initial_cash = 1'000'000.0);

    // ── 심볼 등록 (초기화 시) ──
    int registerSymbol(const std::string& name, double market_price = 0.0);

    // ── 외부 포지션 계산기 → 엔진 ──
    void updatePosition(int sym_id, double qty, double avg_px, double mkt_px);
    void setUnrealizedPnL(double pnl);

    // ── 핵심: 주문 제출 ──
    Result submitOrder(const OrderReq& order);

    // ── 체결 콜백 ──
    void onFill(int order_id, double fill_qty, double fill_price);

    // ── 포트폴리오 조회 ──
    double   cash()          const;
    double   unrealizedPnL() const;
    double   totalEquity()   const;
    Position getPosition(int sym_id) const;

    void printSummary() const;

    uint64_t countAccepted() const { return stat_accepted_.load(std::memory_order_relaxed); }
    uint64_t countRejected() const { return stat_rejected_.load(std::memory_order_relaxed); }
    uint64_t countFilled()   const { return stat_filled_.load(std::memory_order_relaxed); }

private:
    // Per-symbol 격리 상태 (false sharing 방지)
    struct alignas(64) SymbolSlot {
        std::mutex mu;
        Position   pos;
        int        pending_buys = 0;
    };

    // 현금: atomic int64 (×10000 정밀도)로 CAS 가능
    static constexpr int64_t PREC = 10000;
    std::atomic<int64_t> cash_units_;

    double unrealized_pnl_ = 0.0;
    std::mutex pnl_mu_;

    // 심볼 레지스트리 (read-heavy → shared_mutex)
    std::vector<std::unique_ptr<SymbolSlot>> slots_;
    std::unordered_map<std::string, int>    sym_map_;
    mutable std::shared_mutex               registry_mu_;
    int                                     next_sym_id_ = 0;

    // 주문 저장 (체결 콜백용)
    std::unordered_map<int, OrderReq>  orders_;
    std::mutex                         orders_mu_;

    // 통계 (lock-free)
    std::atomic<uint64_t> stat_accepted_{0};
    std::atomic<uint64_t> stat_rejected_{0};
    std::atomic<uint64_t> stat_filled_{0};

    // helpers
    int     resolveSymbol(const std::string& name);
    bool    tryCashReserve(int64_t units);
    void    cashRestore(int64_t units);
    static int64_t toUnits(double v) { return static_cast<int64_t>(v * PREC + 0.5); }
    static double  fromUnits(int64_t u) { return static_cast<double>(u) / PREC; }
};

} // namespace oem
