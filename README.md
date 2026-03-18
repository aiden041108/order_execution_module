# Order Execution Module

C++20 저지연 주문 실행 모듈. 포지션 계산 결과를 받아 최대한 빠르게 주문을 전송하여 알파 신호 감쇠를 방지합니다.

---

## 시스템 개요

```
        ╭──────────────╮    ╭──────────────╮
        │ 포지션 계산기  │    │   알파 모델   │
        ╰──────┬───────╯    ╰──────┬───────╯
               │                   │
               ▼                   ▼
        ◆━━━━━━━━━━━━━━━━━━━━━━━━━━━━━◆
        ┃                             ┃
        ┃        OrderEngine          ┃
        ┃   ┌───────────────────┐     ┃
        ┃   │ 포지션 검증        │     ┃
        ┃   │ Atomic CAS 현금   │     ┃
        ┃   │ 주문 수락/전송     │     ┃
        ┃   └───────────────────┘     ┃
        ┃                             ┃
        ◆━━━━━━━━━━━━━┳━━━━━━━━━━━━━━━◆
                      ┃
                      ▼
              ╔═══════════════╗
              ║ 포지션/현금    ║
              ║   업데이트     ║
              ╚═══════════════╝
```

**포트폴리오 구조:** `total_equity = cash + unrealized_profit + realized_profit`

이 모듈이 관리하는 것: **cash**, **포지션**
외부에서 주입: **unrealized_profit** (via `setUnrealizedPnL()`)
범위 밖: **realized_profit** (외부 시스템)

---

## API

```cpp
// 초기화
OrderEngine engine(1'000'000.0);                     // 초기 현금 (USDT)
int btc = engine.registerSymbol("BTCUSDT", 70000.0); // 심볼 등록

// 포지션 업데이트 (외부 포지션 계산기)
engine.updatePosition(btc, 2.0, 68000.0, 70000.0);  // qty, avg_px, mkt_px
engine.setUnrealizedPnL(4000.0);                     // 미실현 손익

// 주문 제출
OrderReq order{.id=1, .symbol="ETHUSDT", .side=Side::BUY,
               .inst_type=InstType::FUTURES,
               .quantity=10, .price=3500, .leverage=10};
Result r = engine.submitOrder(order);  // → ACCEPTED / REJECT_CASH / REJECT_NO_POSITION

// 체결 콜백
engine.onFill(1, 10.0, 3500.0);       // 포지션 생성, 이후 SELL 가능
```

---

## 속도 최적화 원리

### 1. Atomic CAS 현금 관리 (Lock-Free)
현금 검사와 차감을 **단일 원자 연산**으로 수행. 별도 lock이 필요 없음.

```cpp
bool tryCashReserve(int64_t units) {
    int64_t cur = cash_units_.load(std::memory_order_relaxed);
    while (cur >= units) {
        if (cash_units_.compare_exchange_weak(cur, cur - units,
                std::memory_order_relaxed))
            return true;  // 성공: lock 없이 차감 완료
    }
    return false;  // 잔고 부족
}
```

장점: `std::mutex` 없이 현금 접근 → 심볼 간 현금 경합 시에도 lock-free

### 2. Per-Symbol 격리 상태
각 심볼에 독립된 `std::mutex` + `Position` 할당. `alignas(64)`로 cache line 분리.

```cpp
struct alignas(64) SymbolSlot {
    std::mutex mu;          // 이 심볼 전용 lock
    Position   pos;
    int        pending_buys;
};
```

- 서로 다른 심볼의 주문은 **lock 경합 없이 완전 병렬** 처리
- `alignas(64)`: 인접 심볼 간 false sharing 방지

### 3. 인라인 직접 실행
큐나 스레드풀 없이 호출자 스레드에서 즉시 검증+처리.

```
submitOrder() 핫 패스:
  ① 심볼 조회 (shared_lock 읽기)     ~5ns
  ② per-symbol lock 취득             ~20ns (비경합 시)
  ③ SELL 포지션 검증                  ~5ns
  ④ Atomic CAS 현금 차감              ~10ns
  ⑤ 주문 수락                        ~5ns
  총합: ~50ns (비경합)
```

### 4. 심볼 레지스트리 (ID 기반 조회)
문자열 심볼 → 정수 ID 매핑. 이후 조회는 배열 인덱싱 O(1).
`std::shared_mutex`로 읽기 동시성 확보 (read-heavy 패턴).

---

## 테스트 케이스

### TC1: 심볼 간 순차 틱 주문
- **상황**: BTC 포지션 계산 → ETH FUTURES BUY → SOL OPTIONS BUY → BTC SELL
- **검증**: 각 `submitOrder()` µs 단위 반환, 심볼 간 블로킹 없음
- **결과**: ✅ PASS (3~5 µs)

### TC2: 여러 심볼 동시 주문
- **상황**: 5개 스레드에서 BTC/ETH/BNB/SOL/XRP 동시 주문
- **검증**: 전부 ACCEPTED, 데이터 손상 없음
- **결과**: ✅ PASS (5/5 OK)

### TC3: 매수 체결 전 매도 → 거부
- **상황**: FUTURES BUY 제출 → 체결 전 SELL 시도
- **검증**: `REJECT_NO_POSITION` → `onFill()` 후 SELL 가능
- **결과**: ✅ PASS (FUTURES & OPTIONS 모두)

---

## 빌드 및 실행

```bash
cd quant_order_model
rm -rf build && mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)   # Linux: make -j$(nproc)
./oe_runner        # TC1~TC3 하드코딩 테스트
./dummy_runner     # CSV 더미 데이터 실험
```

요구: CMake ≥ 3.20, C++20 (GCC 10+ / Clang 12+ / MSVC 2022)

---

## 더미 데이터 실험

`dummy_data/` 폴더의 CSV 파일을 로드하여 실험합니다. CSV를 수정하면 다른 시나리오도 즉시 테스트 가능.

### CSV 형식

```csv
id,symbol,side,inst_type,quantity,price,leverage
1,BTCUSDT,BUY,FUTURES,0.5,69500.0,10
```

TC3용 추가 `action` 컬럼: `submit`, `submit_before_fill`, `fill_order_201`, `submit_after_fill`

### 더미 데이터 파일

| 파일 | 내용 |
|------|------|
| `tc1_sequential.csv` | BTC/ETH/SOL/BNB 5건 순차 주문 (FUTURES+OPTIONS) |
| `tc2_simultaneous.csv` | 8개 심볼 8건 동시 제출 |
| `tc3_sell_before_fill.csv` | ETH FUTURES + BTC OPTIONS 체결 전/후 매도 시나리오 |

### 더미 데이터 실험 결과

```
TC1 (CSV): 5개 순차 주문 → 전부 ACCEPTED (2~8 µs)
TC2 (CSV): 8개 스레드 동시 → 8/8 OK (~245 µs total)
TC3 (CSV): 체결 전 매도 REJECT → 체결 후 매도 OK → 4/4 PASS
```

---

## 파일 구조

```
quant_order_model/
├── order_engine.hpp      # OrderEngine 클래스 선언
├── order_engine.cpp      # 구현
├── main.cpp              # TC1~TC3 테스트 러너
├── dummy_runner.cpp      # CSV 더미 데이터 실험 러너
├── dummy_data/
│   ├── tc1_sequential.csv
│   ├── tc2_simultaneous.csv
│   └── tc3_sell_before_fill.csv
├── CMakeLists.txt
└── README.md
```

---

## 테스트 환경

| 항목 | 스펙 |
|------|------|
| CPU | Apple M-series (MacBook Pro) |
| OS | macOS |
| 빌드 | AppleClang 16.0, Release, C++20 |

| TC | 결과 | 비고 |
|----|------|------|
| TC1 Sequential Tick | ✅ PASS | 3~5 µs per call |
| TC2 Simultaneous 5 symbols | ✅ PASS | 5/5 OK |
| TC3 Sell Before Buy Filled | ✅ PASS | REJECT → FILL → OK |
