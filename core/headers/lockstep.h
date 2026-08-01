#pragma once

// ─────────────────────────────────────────────────────────────
//  Deterministic Lockstep Mode (#11) — runtime linter
// ─────────────────────────────────────────────────────────────
// DeterminismLinter es la capa de VERIFICACIÓN runtime del modo lockstep:
//
//   - lock(): sella el world en modo determinista (orden canónico
//     permanente de iteración de arquetipos y consultas).
//   - states_equal(a, b): desync detection bit-exacta entre dos worlds
//     (p.ej. dos clientes en lockstep, o un servidor contra su espejo).
//   - state_hash(w): hash FNV-1a 64 del estado completo del world en
//     orden canónico — el mismo valor para el mismo estado, sin importar
//     el orden de inserción de arquetipos o la plataforma.

#include "ecs.h"

namespace fluxdb {
namespace det {

class DeterminismLinter {
public:
    DeterminismLinter() = default;

    // Sella el modo determinista del world asociado (iteración canónica).
    void lock();
    bool locked() const { return locked_; }

    // Hash FNV-1a 64 del estado completo en orden canónico.
    static uint64_t state_hash(const ecs::World& w);

    // True si los dos worlds tienen el MISMO estado bit-exacto.
    static bool states_equal(const ecs::World& a, const ecs::World& b);

private:
    bool locked_ = false;
};

} // namespace det
} // namespace fluxdb
