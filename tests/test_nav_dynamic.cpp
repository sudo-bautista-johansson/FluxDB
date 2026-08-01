// FluxDB — Feature #18: Incremental Dynamic Navigation Graph Updates
// Parches incrementales del nav graph por obstáculos (sin re-bake global).
#include "../core/headers/nav_dynamic.h"
#include <iostream>
#include <cassert>

using namespace fluxdb::nav;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_graph_build() {
    std::cout << "1. Construcción de grafo indexado por arquetipos...\n";

    DynamicNavGraph g;
    // Grid 2x2 de nodos en celdas.
    uint32_t n00 = g.add_node(0, 0, 0, 0, 0, 0);
    uint32_t n10 = g.add_node(1024, 0, 0, 1, 0, 0);
    uint32_t n01 = g.add_node(0, 0, 1024, 0, 0, 1);
    uint32_t n11 = g.add_node(1024, 0, 1024, 1, 0, 1);

    CHECK(g.node_count() == 4);
    CHECK(g.get_node(n00) != nullptr);
    CHECK(g.get_node(n00)->blocked == false);

    g.add_edge(n00, n10);
    g.add_edge(n10, n11);
    g.add_edge(n00, n01);
    g.add_edge(n01, n11);
    CHECK(g.edge_count() == 4);

    // Consulta por celda.
    std::vector<uint32_t> cell0;
    CHECK(g.nodes_in_cell(0, 0, 0, cell0) == 1);
    CHECK(cell0[0] == n00);
}

static void test_incremental_obstacle_patch() {
    std::cout << "2. Obstáculo bloquea solo el área afectada...\n";

    DynamicNavGraph g;
    uint32_t n00 = g.add_node(0, 0, 0, 0, 0, 0);
    uint32_t n10 = g.add_node(1024, 0, 0, 1, 0, 0);
    uint32_t n01 = g.add_node(0, 0, 1024, 0, 0, 1);
    uint32_t n11 = g.add_node(1024, 0, 1024, 1, 0, 1);
    g.add_edge(n00, n10);
    g.add_edge(n10, n11);
    g.add_edge(n00, n01);
    g.add_edge(n01, n11);

    // Obstáculo en la celda (1,0,0) → bloquea solo n10.
    NavObstacle obs;
    obs.min_x = 1; obs.max_x = 1;
    obs.min_y = 0; obs.max_y = 0;
    obs.min_z = 0; obs.max_z = 0;

    uint64_t seq = 0;
    NavPatch patch = g.apply_obstacle(obs, true, seq);
    CHECK(seq == 1);
    CHECK(patch.block_nodes.size() == 1);
    CHECK(patch.block_nodes[0] == n10);
    CHECK(patch.removed_edges.size() == 2); // n00-n10 y n10-n11

    // Solo n10 quedó bloqueado.
    CHECK(g.get_node(n10)->blocked);
    CHECK(!g.get_node(n00)->blocked);
    CHECK(!g.get_node(n11)->blocked);

    // Reachability: n00→n10 cortado, n00→n01 sigue.
    CHECK(!g.is_reachable(n00, n10));
    CHECK(g.is_reachable(n00, n01));

    // Desbloquear: patch inverso.
    NavPatch un = g.apply_obstacle(obs, false, seq);
    CHECK(seq == 2);
    CHECK(un.unblock_nodes.size() == 1);
    CHECK(un.unblock_nodes[0] == n10);
    CHECK(g.is_reachable(n00, n10));
}

static void test_patch_replication() {
    std::cout << "3. Parche replicable en otro grafo (delta)...\n";

    DynamicNavGraph server;
    DynamicNavGraph client;

    // Ambos arrancan con la misma topología (mismos ids).
    uint32_t a = server.add_node(0, 0, 0, 0, 0, 0);
    uint32_t b = server.add_node(1024, 0, 0, 1, 0, 0);
    server.add_edge(a, b);
    client.add_node(0, 0, 0, 0, 0, 0);
    client.add_node(1024, 0, 0, 1, 0, 0);
    client.add_edge(a, b);

    NavObstacle obs;
    obs.min_x = 1; obs.max_x = 1; obs.min_y = 0; obs.max_y = 0; obs.min_z = 0; obs.max_z = 0;
    uint64_t seq = 0;
    NavPatch patch = server.apply_obstacle(obs, true, seq);

    // El cliente aplica el MISMO parche → estado idéntico.
    client.apply_patch(patch);
    CHECK(client.get_node(b)->blocked);
    CHECK(!client.is_reachable(a, b));
    CHECK(server.is_reachable(a, b) == client.is_reachable(a, b));

    // Aplicar dos veces el mismo bloqueo es idempotente (sin nodos duplicados).
    uint64_t seq2 = 0;
    NavPatch again = server.apply_obstacle(obs, true, seq2);
    CHECK(again.block_nodes.empty()); // ya estaba bloqueado
}

static void test_cost_scoped_to_area() {
    std::cout << "4. El patch es proporcional al área, no al mundo...\n";

    DynamicNavGraph g;
    // Mundo grande: 100 nodos lejanos.
    for (int i = 0; i < 100; ++i) {
        g.add_node(static_cast<float>(i) * 1024, 0, 0, static_cast<int16_t>(i), 0, 0);
    }

    NavObstacle small;
    small.min_x = 2; small.max_x = 2; small.min_y = 0; small.max_y = 0; small.min_z = 0; small.max_z = 0;
    uint64_t seq = 0;
    NavPatch patch = g.apply_obstacle(small, true, seq);
    // Solo el nodo en la celda 2 queda afectado, no los 99 restantes.
    CHECK(patch.block_nodes.size() == 1);
    CHECK(patch.block_nodes.size() < 100);
}

int main() {
    std::cout << "--- Starting FluxDB Dynamic Nav Graph Test (#18) ---\n";
    test_graph_build();
    test_incremental_obstacle_patch();
    test_patch_replication();
    test_cost_scoped_to_area();
    std::cout << "--- DYNAMIC NAV GRAPH TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}