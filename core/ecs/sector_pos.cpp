// Feature #19: Infinite World Origin Rebasing — Sector-Relative Positions.
// La implementación completa de SectorPos y SpatialSectorGrid vive en
// core/headers/sector_pos.h (header-only, aritmética inline). Este archivo
// es la unidad de compilación del módulo (reserva futura de símbolos no
// inline).

#include "../headers/sector_pos.h"

namespace fluxdb {
namespace ecs {

// Punto de anclaje del módulo: garantiza que el header se compila como parte
// del core (detecta errores de header-only en el CI del propio engine).
void sector_pos_compile_anchor() {
    SectorPos p = SectorPos::from_world(0.0f, 0.0f, 0.0f);
    (void)p;
}

} // namespace ecs
} // namespace fluxdb
