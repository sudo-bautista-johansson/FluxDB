#pragma once

// ─────────────────────────────────────────────────────────────
//  Deterministic Lockstep Mode (#11) — trait + linter compile-time
// ─────────────────────────────────────────────────────────────
// El modo lockstep requiere que TODOS los componentes de simulación sean
// deterministas: enteros o det::Fix32 (nunca float/double). Este header
// expone:
//
//   det::is_deterministic_v<T>      — trait compile-time
//   det::static_assert_deterministic<T>() — linter compile-time (static_assert)
//   FLUXDB_STATIC_ASSERT_DETERMINISTIC(T) — macro conveniente
//
// Definir FLUXDB_DETERMINISM_MODE=1 en el build (CMake) marca el modo
// determinismo-locked: además del linter compile-time, los consumidores de
// iteración canónica del World (#11) están garantizados ordenados.
//
// El linter RUNTIME (det::DeterminismLinter, determinism.cpp) verifica la
// igualdad bit-exacta de dos worlds (desync detection) y sella el modo.

#include <cstdint>
#include <type_traits>
#include "fixed.h"

#ifndef FLUXDB_DETERMINISM_MODE
#define FLUXDB_DETERMINISM_MODE 0
#endif

namespace fluxdb {
namespace det {

// Trait compile-time: ¿el tipo es seguro para lockstep?
template <typename T, typename = void>
struct is_deterministic : std::false_type {};

template <> struct is_deterministic<int8_t>   : std::true_type {};
template <> struct is_deterministic<uint8_t>  : std::true_type {};
template <> struct is_deterministic<int16_t>  : std::true_type {};
template <> struct is_deterministic<uint16_t> : std::true_type {};
template <> struct is_deterministic<int32_t>  : std::true_type {};
template <> struct is_deterministic<uint32_t> : std::true_type {};
template <> struct is_deterministic<int64_t>  : std::true_type {};
template <> struct is_deterministic<uint64_t> : std::true_type {};
template <> struct is_deterministic<bool>     : std::true_type {};
template <> struct is_deterministic<char>     : std::true_type {};
template <> struct is_deterministic<Fix32>    : std::true_type {};

// float/double NO son deterministas: las transcendentales FP (sin/cos/sqrt)
// pueden dar el último bit distinto entre plataformas → desync en lockstep.

template <typename T>
inline constexpr bool is_deterministic_v = is_deterministic<T>::value;

// Linter compile-time: falla la compilación si T no es determinista.
template <typename T>
constexpr void static_assert_deterministic() {
    static_assert(is_deterministic_v<T>,
                  "FluxDB #11 (lockstep): el tipo no es determinista — "
                  "usa det::Fix32 o enteros en componentes de simulación.");
}

} // namespace det
} // namespace fluxdb

#define FLUXDB_STATIC_ASSERT_DETERMINISTIC(T) ::fluxdb::det::static_assert_deterministic<T>()
