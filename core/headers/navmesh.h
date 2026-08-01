#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #17: Streaming NavMesh Tied to World Streaming
//  (Navigation stack)
// ─────────────────────────────────────────────────────────────
// NavMesh por tiles, indexado por las MISMAS coordenadas de chunk
// que el streaming de mundo (#20). La carga/descarga de tiles de
// navegación usa el mismo budget/prioridad, garantizando lockstep
// con render/collision. Un pathfinding chequea la residencia de los
// tiles que atraviesa y puede pedir carga just-in-time.

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace fluxdb {
namespace nav {

// Coordenada de tile de navegación (misma convención que el chunk
// de streaming: origen * 1024 unidades).
struct NavTileCoord {
    int32_t cx = 0, cy = 0, cz = 0;
    bool operator==(const NavTileCoord& o) const { return cx == o.cx && cy == o.cy && cz == o.cz; }
    uint64_t hash() const {
        uint64_t h = 1469598103934665603ull;
        const uint32_t v[3] = { static_cast<uint32_t>(cx), static_cast<uint32_t>(cy), static_cast<uint32_t>(cz) };
        for (int i = 0; i < 3; ++i) { h ^= v[i]; h *= 1099511628211ull; }
        return h;
    }
};

struct NavTileCoordHash {
    std::size_t operator()(const NavTileCoord& c) const { return c.hash(); }
};

// Nodo de navegación dentro de un tile.
struct NavNode {
    uint32_t id = 0;
    float x = 0, y = 0, z = 0;      // posición dentro del tile (local)
};

// Un tile de navmesh: nodos + vecindad intra-tile.
struct NavTile {
    NavTileCoord coord;
    bool resident = false;
    std::vector<NavNode> nodes;
    // Conectividad con tiles vecinos: par de (tile vecino, puerta).
    struct Edge { NavTileCoord to; uint16_t a; uint16_t b; };
    std::vector<Edge> edges;
};

// Estado de residencia (mismo concepto que ChunkState en #20).
enum class TileResidency : uint8_t {
    DORMANT,
    LOADING,
    ACTIVE
};

// Gestor de navmesh streamed. Comparte convención de coords con el
// StreamingManager (#20): la distancia usa chunk_size = 1024.
class NavMeshStreamer {
public:
    explicit NavMeshStreamer(float chunk_size = 1024.0f) : chunk_size_(chunk_size) {}

    NavTileCoord coord_for(float x, float y, float z) const {
        NavTileCoord c;
        c.cx = static_cast<int32_t>(std::floor(x / chunk_size_));
        c.cy = static_cast<int32_t>(std::floor(y / chunk_size_));
        c.cz = static_cast<int32_t>(std::floor(z / chunk_size_));
        return c;
    }

    // Solicitud de carga (just-in-time): un pathfinding pide un tile.
    // Devuelve true si ya está residente/activo.
    bool request_load(const NavTileCoord& c) {
        auto it = tiles_.find(c);
        if (it != tiles_.end() && it->second.resident) return true;
        load_pending_.push_back(c);
        return false;
    }

    // El sistema de streaming materializa el tile (desde disco/etc.).
    // El caller provee los nodos; el gestor guarda residencia.
    void provide_tile(const NavTile& tile) {
        NavTile t = tile;
        t.resident = true;
        tiles_[tile.coord] = std::move(t);
        auto& rem = load_pending_;
        rem.erase(std::remove_if(rem.begin(), rem.end(),
                  [&](const NavTileCoord& c) { return c == tile.coord; }), rem.end());
    }

    void unload(const NavTileCoord& c) {
        auto it = tiles_.find(c);
        if (it != tiles_.end()) it->second.resident = false;
    }

    bool is_resident(const NavTileCoord& c) const {
        auto it = tiles_.find(c);
        return it != tiles_.end() && it->second.resident;
    }

    NavTile* get(const NavTileCoord& c) {
        auto it = tiles_.find(c);
        return (it != tiles_.end() && it->second.resident) ? &it->second : nullptr;
    }

    // Peticiones pendientes de carga (para el sistema de streaming).
    const std::vector<NavTileCoord>& pending() const { return load_pending_; }
    void clear_pending() { load_pending_.clear(); }

    size_t resident_count() const {
        size_t n = 0;
        for (auto& [c, t] : tiles_) if (t.resident) ++n;
        return n;
    }

    // Tiles que deberían estar activos según un punto de interés:
    // devuelve los coords dentro de `radius` chunks del punto.
    void tiles_in_radius(float x, float y, float z, int radius, std::vector<NavTileCoord>& out) const {
        NavTileCoord center = coord_for(x, y, z);
        for (int dz = -radius; dz <= radius; ++dz)
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx) {
                    NavTileCoord c{ center.cx + dx, center.cy + dy, center.cz + dz };
                    out.push_back(c);
                }
    }

private:
    float chunk_size_;
    std::unordered_map<NavTileCoord, NavTile, NavTileCoordHash> tiles_;
    std::vector<NavTileCoord> load_pending_;
};

// ── Pathfinding sobre tiles streamed ─────────────────────────

// Punto de ruta resultado.
struct NavPathPoint {
    float x, y, z;
};

// Busca camino entre `from` y `to` a través de los tiles RESIDENTES.
// Si algún tile intermedio no está residente, lo marca como carga
// pendiente (just-in-time) y devuelve false con `needs_streaming=true`.
class NavPathfinder {
public:
    NavPathfinder(NavMeshStreamer& streamer, float step = 64.0f) : streamer_(streamer), step_(step) {}

    bool find_path(float from_x, float from_y, float from_z,
                   float to_x, float to_y, float to_z,
                   std::vector<NavPathPoint>& out, bool& needs_streaming) {
        needs_streaming = false;
        out.clear();

        NavTileCoord from_t = streamer_.coord_for(from_x, from_y, from_z);
        NavTileCoord to_t = streamer_.coord_for(to_x, to_y, to_z);

        if (!streamer_.is_resident(from_t)) {
            streamer_.request_load(from_t);
            needs_streaming = true;
            return false;
        }
        if (!streamer_.is_resident(to_t)) {
            streamer_.request_load(to_t);
            needs_streaming = true;
            return false;
        }

        // Camino directo simple en línea recta: suficiente para tiles
        // contiguos; si hay un tile no-residente en medio, pide carga.
        float dx = to_x - from_x, dy = to_y - from_y, dz = to_z - from_z;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dist <= step_) {
            out.push_back({to_x, to_y, to_z});
            return true;
        }

        int steps = static_cast<int>(dist / step_);
        for (int i = 1; i <= steps; ++i) {
            float t = static_cast<float>(i) / steps;
            float px = from_x + dx * t;
            float py = from_y + dy * t;
            float pz = from_z + dz * t;
            NavTileCoord pc = streamer_.coord_for(px, py, pz);
            if (!streamer_.is_resident(pc)) {
                streamer_.request_load(pc);
                needs_streaming = true;
                out.clear();
                return false;
            }
            out.push_back({px, py, pz});
        }
        out.push_back({to_x, to_y, to_z});
        return true;
    }

private:
    NavMeshStreamer& streamer_;
    float step_;
};

} // namespace nav
} // namespace fluxdb
