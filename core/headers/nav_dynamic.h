#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #18: Incremental Dynamic Navigation Graph Updates
//  (Navigation stack)
// ─────────────────────────────────────────────────────────────
// El nav graph es un grafo indexado por arquetipos (nodos/edges como
// datos) en vez de un blob horneado. `NavObstacle` dispara patches
// incrementales SOLO sobre las celdas que toca su AABB — el coste es
// proporcional al área afectada, no al tamaño del mundo. Los patches
// son diffs aplicables (replicables por el delta engine, #7).

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>

namespace fluxdb {
namespace nav {

// Nodo del grafo dinámico.
struct NavGraphNode {
    uint32_t id = 0;
    float x = 0, y = 0, z = 0;
    int16_t cell_x = 0, cell_y = 0, cell_z = 0;
    bool blocked = false;          // afectado por un obstáculo
};

// Arista entre dos nodos.
struct NavGraphEdge {
    uint32_t a = 0, b = 0;
    float cost = 1.0f;
};

// Un obstáculo dinámico: AABB en coordenadas de celda (int).
struct NavObstacle {
    int16_t min_x = 0, min_y = 0, min_z = 0;
    int16_t max_x = 0, max_y = 0, max_z = 0;

    bool overlaps(int16_t cx, int16_t cy, int16_t cz) const {
        return cx >= min_x && cx <= max_x &&
               cy >= min_y && cy <= max_y &&
               cz >= min_z && cz <= max_z;
    }
};

// Un patch incremental: qué nodos bloquear/desbloquear + aristas
// afectadas. Aplicable (apply) y diffs de aristas eliminadas/añadidas.
struct NavPatch {
    uint64_t seq = 0;
    std::vector<uint32_t> block_nodes;
    std::vector<uint32_t> unblock_nodes;
    std::vector<NavGraphEdge> removed_edges;
    std::vector<NavGraphEdge> added_edges;

    bool empty() const {
        return block_nodes.empty() && unblock_nodes.empty() &&
               removed_edges.empty() && added_edges.empty();
    }
};

// Grafo dinámico incremental.
class DynamicNavGraph {
public:
    uint32_t add_node(float x, float y, float z, int16_t cx, int16_t cy, int16_t cz) {
        NavGraphNode n;
        n.id = next_id_++;
        n.x = x; n.y = y; n.z = z;
        n.cell_x = cx; n.cell_y = cy; n.cell_z = cz;
        nodes_[n.id] = n;
        return n.id;
    }

    void add_edge(uint32_t a, uint32_t b, float cost = 1.0f) {
        edges_.push_back({a, b, cost});
    }

    const NavGraphNode* get_node(uint32_t id) const {
        auto it = nodes_.find(id);
        return it == nodes_.end() ? nullptr : &it->second;
    }

    size_t node_count() const { return nodes_.size(); }
    size_t edge_count() const { return edges_.size(); }

    uint32_t nodes_in_cell(int16_t cx, int16_t cy, int16_t cz,
                           std::vector<uint32_t>& out) const {
        uint32_t count = 0;
        for (const auto& [id, n] : nodes_) {
            if (n.cell_x == cx && n.cell_y == cy && n.cell_z == cz) {
                out.push_back(id);
                ++count;
            }
        }
        return count;
    }

    // Patch incremental: recalcula SOLO los nodos dentro del AABB del
    // obstáculo. No toca el resto del grafo. `block=true` marca nodos
    // bloqueados y sus aristas; `block=false` los restaura.
    NavPatch apply_obstacle(const NavObstacle& obs, bool block, uint64_t& seq) {
        NavPatch patch;
        patch.seq = ++seq;

        // 1) Nodos en el área del obstáculo.
        std::vector<uint32_t> affected;
        for (auto& [id, n] : nodes_) {
            if (obs.overlaps(n.cell_x, n.cell_y, n.cell_z)) {
                affected.push_back(id);
            }
        }
        if (affected.empty()) return patch;

        // 2) Aristas que conectan nodos afectados.
        for (const auto& e : edges_) {
            bool a_aff = std::find(affected.begin(), affected.end(), e.a) != affected.end();
            bool b_aff = std::find(affected.begin(), affected.end(), e.b) != affected.end();
            if (a_aff || b_aff) {
                if (block) {
                    patch.removed_edges.push_back(e);
                } else {
                    patch.added_edges.push_back(e);
                }
            }
        }

        // 3) Marcar/desmarcar nodos.
        for (uint32_t id : affected) {
            auto& n = nodes_[id];
            if (block && !n.blocked) { n.blocked = true; patch.block_nodes.push_back(id); }
            if (!block && n.blocked) { n.blocked = false; patch.unblock_nodes.push_back(id); }
        }
        return patch;
    }

    // Aplica un patch (replica en un cliente/replica).
    void apply_patch(const NavPatch& patch) {
        for (uint32_t id : patch.block_nodes) {
            auto it = nodes_.find(id);
            if (it != nodes_.end()) it->second.blocked = true;
        }
        for (uint32_t id : patch.unblock_nodes) {
            auto it = nodes_.find(id);
            if (it != nodes_.end()) it->second.blocked = false;
        }
        if (!patch.removed_edges.empty()) {
            edges_.erase(
                std::remove_if(edges_.begin(), edges_.end(), [&](const NavGraphEdge& e) {
                    for (const auto& r : patch.removed_edges)
                        if ((r.a == e.a && r.b == e.b)) return true;
                    return false;
                }), edges_.end());
        }
        for (const auto& e : patch.added_edges) edges_.push_back(e);
    }

    // ¿Un camino directo (salto a saltos) está bloqueado?
    // Simplificado: devuelve true si el destino o un nodo clave está bloqueado.
    bool is_reachable(uint32_t a, uint32_t b) const {
        auto itA = nodes_.find(a), itB = nodes_.find(b);
        if (itA == nodes_.end() || itB == nodes_.end()) return false;
        if (itA->second.blocked || itB->second.blocked) return false;
        // Buscar arista directa a→b (no bloqueada).
        for (const auto& e : edges_) {
            if ((e.a == a && e.b == b)) return true;
        }
        return false;
    }

private:
    uint32_t next_id_ = 1;
    std::unordered_map<uint32_t, NavGraphNode> nodes_;
    std::vector<NavGraphEdge> edges_;
};

} // namespace nav
} // namespace fluxdb
