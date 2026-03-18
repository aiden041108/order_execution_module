/**
 * order_engine.cpp
 *
 * 저지연 주문 실행 엔진 — 구현
 */

#include "order_engine.hpp"

#include <algorithm>

namespace oem {

OrderEngine::OrderEngine(double initial_cash)
    : cash_units_(toUnits(initial_cash)) {
    slots_.reserve(64);
    for (int i = 0; i < 64; ++i)
        slots_.push_back(std::make_unique<SymbolSlot>());
}

int OrderEngine::registerSymbol(const std::string& name, double market_price) {
    std::unique_lock lk(registry_mu_);
    auto it = sym_map_.find(name);
    if (it != sym_map_.end()) return it->second;

    int id = next_sym_id_++;
    sym_map_[name] = id;

    if (id >= static_cast<int>(slots_.size())) {
        size_t new_size = slots_.size() * 2;
        slots_.reserve(new_size);
        for (size_t i = slots_.size(); i < new_size; ++i)
            slots_.push_back(std::make_unique<SymbolSlot>());
    }

    slots_[id]->pos.market_price = market_price;
    return id;
}

void OrderEngine::updatePosition(int sym_id, double qty, double avg_px, double mkt_px) {
    if (sym_id < 0 || sym_id >= static_cast<int>(slots_.size())) return;

    std::lock_guard lk(slots_[sym_id]->mu);
    slots_[sym_id]->pos.net_qty      = qty;
    slots_[sym_id]->pos.avg_price    = avg_px;
    slots_[sym_id]->pos.market_price = mkt_px;
}

void OrderEngine::setUnrealizedPnL(double pnl) {
    std::lock_guard lk(pnl_mu_);
    unrealized_pnl_ = pnl;
}

// ── submitOrder: 핵심 핫 패스 ──
//
// 1. 심볼 조회 (shared_lock, 읽기 동시 허용)
// 2. per-symbol lock 취득 (다른 심볼과 경합 없음)
// 3. SELL 검증: 실제 포지션 확인
// 4. 현금 검증: atomic CAS (lock 없이)
// 5. 주문 수락
//
Result OrderEngine::submitOrder(const OrderReq& order) {
    int sym_id = resolveSymbol(order.symbol);

    // Per-symbol lock — 이 심볼만 잠금, 다른 심볼은 완전 병렬
    std::lock_guard sym_lk(slots_[sym_id]->mu);
    auto& slot = *slots_[sym_id];

    // ── SELL 검증: 실제 포지션이 있어야 매도 가능 ──
    if (order.side == Side::SELL) {
        if (!slot.pos.has_position() || slot.pos.net_qty < order.quantity - 1e-9) {
            stat_rejected_.fetch_add(1, std::memory_order_relaxed);
            std::printf("  [REJECT_NO_POSITION] id=%d %s SELL %s qty=%.2f "
                        "(have=%.2f)\n",
                        order.id, to_str(order.inst_type),
                        order.symbol.c_str(), order.quantity, slot.pos.net_qty);
            return Result::REJECT_NO_POSITION;
        }
    }

    // ── 현금 검증: atomic CAS — lock 없이 차감 ──
    if (order.side == Side::BUY) {
        int64_t margin_units = toUnits(order.margin());
        if (!tryCashReserve(margin_units)) {
            stat_rejected_.fetch_add(1, std::memory_order_relaxed);
            std::printf("  [REJECT_CASH] id=%d margin=%.2f cash=%.2f\n",
                        order.id, order.margin(), cash());
            return Result::REJECT_CASH;
        }
    }

    // ── 주문 수락 ──
    if (order.side == Side::BUY)
        slot.pending_buys++;

    {
        std::lock_guard lk(orders_mu_);
        orders_[order.id] = order;
    }

    stat_accepted_.fetch_add(1, std::memory_order_relaxed);
    std::printf("  [ACCEPTED] id=%d %s %s %s qty=%.2f px=%.2f lev=%d\n",
                order.id, to_str(order.inst_type), to_str(order.side),
                order.symbol.c_str(), order.quantity, order.price,
                order.leverage);

    return Result::ACCEPTED;
}

// ── 체결 콜백 ──
void OrderEngine::onFill(int order_id, double fill_qty, double fill_price) {
    OrderReq order;
    {
        std::lock_guard lk(orders_mu_);
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return;
        order = it->second;
        orders_.erase(it);
    }

    int sym_id = resolveSymbol(order.symbol);
    std::lock_guard sym_lk(slots_[sym_id]->mu);
    auto& slot = *slots_[sym_id];

    if (order.side == Side::BUY) {
        slot.pending_buys = std::max(0, slot.pending_buys - 1);

        double old_qty = slot.pos.net_qty;
        double new_qty = old_qty + fill_qty;
        if (new_qty > 1e-9) {
            slot.pos.avg_price =
                (slot.pos.avg_price * old_qty + fill_price * fill_qty) / new_qty;
        }
        slot.pos.net_qty      = new_qty;
        slot.pos.market_price = fill_price;
    } else {
        slot.pos.net_qty -= fill_qty;
        if (slot.pos.net_qty < 1e-9) {
            slot.pos.net_qty   = 0.0;
            slot.pos.avg_price = 0.0;
        }
        // SELL 체결 → 매도 대금 현금 복원
        cashRestore(toUnits(fill_qty * fill_price));
    }

    stat_filled_.fetch_add(1, std::memory_order_relaxed);
    std::printf("  [FILLED] id=%d %s %s %s qty=%.2f px=%.2f | pos=%.2f\n",
                order_id, to_str(order.inst_type), to_str(order.side),
                order.symbol.c_str(), fill_qty, fill_price,
                slot.pos.net_qty);
}

double OrderEngine::cash() const {
    return fromUnits(cash_units_.load(std::memory_order_relaxed));
}

double OrderEngine::unrealizedPnL() const {
    return unrealized_pnl_;
}

double OrderEngine::totalEquity() const {
    return cash() + unrealized_pnl_;
}

Position OrderEngine::getPosition(int sym_id) const {
    if (sym_id < 0 || sym_id >= static_cast<int>(slots_.size()))
        return {};
    return slots_[sym_id]->pos;  // snapshot
}

void OrderEngine::printSummary() const {
    std::printf("\n+--------------------------------------+\n");
    std::printf("|         Portfolio Summary             |\n");
    std::printf("+--------------------------------------+\n");
    std::printf("|  Cash            : %.2f\n", cash());
    std::printf("|  Unrealized P&L  : %.2f\n", unrealized_pnl_);
    std::printf("|  Total Equity    : %.2f\n", totalEquity());
    std::printf("|  Accepted        : %llu\n",
                static_cast<unsigned long long>(countAccepted()));
    std::printf("|  Rejected        : %llu\n",
                static_cast<unsigned long long>(countRejected()));
    std::printf("|  Filled          : %llu\n",
                static_cast<unsigned long long>(countFilled()));
    std::printf("+--------------------------------------+\n");
}

// ── Internal ──

int OrderEngine::resolveSymbol(const std::string& name) {
    // 읽기 경로: shared_lock → 동시 읽기 허용
    {
        std::shared_lock lk(registry_mu_);
        auto it = sym_map_.find(name);
        if (it != sym_map_.end()) return it->second;
    }
    // 없으면 등록 (드문 경로)
    return registerSymbol(name);
}

// Atomic CAS: lock 없이 현금 차감
// compare_exchange_weak 루프 — 경합 시 재시도
bool OrderEngine::tryCashReserve(int64_t units) {
    int64_t cur = cash_units_.load(std::memory_order_relaxed);
    while (cur >= units) {
        if (cash_units_.compare_exchange_weak(
                cur, cur - units, std::memory_order_relaxed))
            return true;
        // cur is updated by compare_exchange_weak on failure
    }
    return false;
}

void OrderEngine::cashRestore(int64_t units) {
    cash_units_.fetch_add(units, std::memory_order_relaxed);
}

} // namespace oem
