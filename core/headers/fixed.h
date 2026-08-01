#pragma once

// ─────────────────────────────────────────────────────────────
//  Deterministic Lockstep Mode (#11) — fixed-point math
// ─────────────────────────────────────────────────────────────
// Fix32 es un punto fijo Q16.16 (1 signo + 15 enteros + 16 fraccionarios).
// TODAS las operaciones son aritmética entera pura: bit-exacta e idéntica
// en cualquier plataforma/compilador. Es el tipo canónico de los
// componentes de simulación en modo lockstep (en vez de float, que puede
// producir desyncs por diferencias de FP entre plataformas).
//
// Con BANDWIDTH en mente (#10): 4 bytes por campo, como float, pero
// determinista.
//
// Determinismo de conversión: la conversión desde float usa solo lround
// (IEEE-754 round-to-nearest) — el MISMO resultado en cualquier plataforma
// IEEE (sin fast-math). La tabla de trigonometría se genera una vez con
// std::sin (IEEE determinista); el runtime NO vuelve a tocar FP.

#include <cstdint>
#include <cmath>
#include <limits>

namespace fluxdb {
namespace det {

// Q16.16: valor = raw / 65536
class Fix32 {
public:
    static constexpr int32_t SHIFT = 16;
    static constexpr int64_t SCALE = 1LL << SHIFT;

    constexpr Fix32() : raw_(0) {}
    // Constructor crudo: reinterpreta los 32 bits como Q16.16.
    explicit constexpr Fix32(int32_t raw) : raw_(raw) {}

    // Determinista (lround = IEEE round-to-nearest-even, mismo resultado en
    // toda plataforma IEEE-754).
    Fix32(float f) : raw_(static_cast<int32_t>(std::lroundf(f * 65536.0f))) {}
    Fix32(double d) : raw_(static_cast<int32_t>(std::lround(d * 65536.0))) {}

    static Fix32 from_raw(int32_t raw) { return Fix32(raw); }
    static Fix32 from_float(float f) { return Fix32(f); }
    static Fix32 from_int(int v) { return Fix32(static_cast<int32_t>(static_cast<int64_t>(v) << SHIFT)); }

    int32_t raw() const { return raw_; }
    float to_float() const { return static_cast<float>(raw_) / 65536.0f; }
    double to_double() const { return static_cast<double>(raw_) / 65536.0; }
    int to_int() const { return raw_ >> SHIFT; }

    Fix32 operator+() const { return *this; }
    Fix32 operator-() const { return Fix32(static_cast<int32_t>(0 - static_cast<int64_t>(raw_))); }
    Fix32 operator+(Fix32 o) const { return Fix32(raw_ + o.raw_); }
    Fix32 operator-(Fix32 o) const { return Fix32(raw_ - o.raw_); }

    // Multiplicación entera con redondeo: (a*b + 2^15) >> 16.
    // a,b ∈ [2^31, 2^31) → producto ∈ [2^62, 2^62): cabe en int64.
    Fix32 operator*(Fix32 o) const {
        return Fix32(static_cast<int32_t>(((static_cast<int64_t>(raw_) * o.raw_) + (SCALE >> 1)) >> SHIFT));
    }

    // División entera: (a << 16) / b. b == 0 → satura (nunca divide por 0).
    Fix32 operator/(Fix32 o) const {
        if (o.raw_ == 0) return Fix32(std::numeric_limits<int32_t>::max());
        return Fix32(static_cast<int32_t>((static_cast<int64_t>(raw_) << SHIFT) / o.raw_));
    }

    Fix32& operator+=(Fix32 o) { raw_ += o.raw_; return *this; }
    Fix32& operator-=(Fix32 o) { raw_ -= o.raw_; return *this; }
    Fix32& operator*=(Fix32 o) { *this = *this * o; return *this; }
    Fix32& operator/=(Fix32 o) { *this = *this / o; return *this; }

    bool operator==(Fix32 o) const { return raw_ == o.raw_; }
    bool operator!=(Fix32 o) const { return raw_ != o.raw_; }
    bool operator<(Fix32 o) const { return raw_ < o.raw_; }
    bool operator<=(Fix32 o) const { return raw_ <= o.raw_; }
    bool operator>(Fix32 o) const { return raw_ > o.raw_; }
    bool operator>=(Fix32 o) const { return raw_ >= o.raw_; }

    Fix32 abs() const { return Fix32(raw_ < 0 ? static_cast<int32_t>(0 - static_cast<int64_t>(raw_)) : raw_); }

    // Raíz cuadrada entera (Newton sobre uint64): raw = isqrt(raw << 16).
    Fix32 sqrt() const {
        if (raw_ <= 0) return Fix32(0);
        uint64_t v = static_cast<uint64_t>(static_cast<uint32_t>(raw_)) << 16;
        uint64_t x = v, y = (x + 1) >> 1;
        while (y < x) {
            x = y;
            y = (x + v / x) >> 1;
        }
        return Fix32(static_cast<int32_t>(x));
    }

    // Seno/coseno deterministas: tabla de 1024 entradas por cuadrante de
    // [0, π/2] generada UNA vez con std::sin (IEEE determinista) y
    // evaluación por interpolación lineal ENTERA en runtime (sin FP).
    Fix32 sin() const;
    Fix32 cos() const;

    // Constantes en Q16.16 (round(π × 65536)).
    static constexpr int32_t PI_RAW = 205887;
    static constexpr int32_t TAU_RAW = 411775;
    static constexpr int32_t HALF_PI_RAW = 102943;
    static Fix32 pi() { return Fix32(PI_RAW); }
    static Fix32 tau() { return Fix32(TAU_RAW); }

private:
    int32_t raw_;
};

// Tabla de senos [0, π/2] en 1024 pasos (Q16.16). Generación única con
// std::sin — IEEE-754 determinista en cualquier plataforma.
class SinTable {
public:
    static const SinTable& get();
    const int32_t* entries() const { return table_; }
private:
    SinTable();
    int32_t table_[1025];
};

inline Fix32 Fix32::sin() const {
    const SinTable& t = SinTable::get();
    // Reducción de ángulo entera: n ∈ [0, TAU_RAW).
    uint64_t n = static_cast<uint64_t>(static_cast<uint32_t>(raw_)) % static_cast<uint64_t>(TAU_RAW);
    uint32_t half = static_cast<uint32_t>(HALF_PI_RAW);
    uint32_t quad = static_cast<uint32_t>(n / half);
    uint32_t rem;
    switch (quad) {
    case 0: rem = static_cast<uint32_t>(n); break;              // x ∈ [0, π/2]
    case 1: rem = 2u * half - static_cast<uint32_t>(n); break;  // π - x ∈ [0, π/2]
    case 2: rem = static_cast<uint32_t>(n) - 2u * half; break;  // x - π ∈ [0, π/2]
    default: rem = static_cast<uint32_t>(TAU_RAW) - static_cast<uint32_t>(n); break; // 2π - x
    }
    // Índice [0, 1024] + interpolación lineal entera.
    uint32_t idx = (rem * 1024u) / half;
    uint32_t frac_rem = (rem * 1024u) % half;
    int32_t interp;
    if (idx >= 1024u) {
        interp = t.entries()[1024];
    } else {
        int32_t y0 = t.entries()[idx];
        int32_t y1 = t.entries()[idx + 1];
        uint32_t frac = static_cast<uint32_t>((static_cast<uint64_t>(frac_rem) << 16) / half);
        interp = static_cast<int32_t>((static_cast<int64_t>(y0) * (65536 - frac) +
                                       static_cast<int64_t>(y1) * frac) >> 16);
    }
    return (quad == 0 || quad == 1) ? Fix32(interp) : Fix32(-interp);
}

inline Fix32 Fix32::cos() const {
    // cos(x) = sin(x + π/2)
    return Fix32(raw_ + HALF_PI_RAW).sin();
}

// PRNG determinista (xorshift64*). Misma semilla → misma secuencia en
// cualquier plataforma. Es la ÚNICA fuente de aleatoriedad permitida en
// simulación lockstep (nunca rand()/std::random_device).
class FixedRandom {
public:
    explicit FixedRandom(uint64_t seed = 0x9E3779B97F4A7C15ULL) : state_(seed ? seed : 1) {}

    uint64_t next_u64() {
        uint64_t x = state_;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        state_ = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    uint32_t next_u32() { return static_cast<uint32_t>(next_u64() >> 32); }

    // Entero uniforme en [min, max] (inclusive).
    int next_int(int min, int max) {
        uint32_t range = static_cast<uint32_t>(max - min) + 1u;
        return min + static_cast<int>(next_u32() % range);
    }

    // Q16.16 uniforme en [min, max).
    Fix32 next_fix(Fix32 min, Fix32 max) {
        uint64_t span = static_cast<uint64_t>(static_cast<uint32_t>(max.raw()) - static_cast<uint32_t>(min.raw()));
        uint64_t r = (static_cast<uint64_t>(next_u32()) << 32) | next_u32();
        return Fix32(min.raw() + static_cast<int32_t>(r % span));
    }

    Fix32 next_fix01() { return Fix32(static_cast<int32_t>(next_u32() & 0xFFFFu)); }

private:
    uint64_t state_;
};

} // namespace det
} // namespace fluxdb
