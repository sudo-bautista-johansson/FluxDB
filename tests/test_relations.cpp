#include "../core/headers/ecs.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

// ─────────────────────────────────────────
//  Roadmap #6: Native Entity Relationship Graphs
//  (src, kind, dst) triples: forward + backward indexed, payloads,
//  versionado unificado con #4, limpieza en despawn/clear_all
// ─────────────────────────────────────────

using namespace fluxdb::ecs;

struct Position { float x, y, z; };
struct Health   { int hp; };

int main() {
    std::cout << "--- Starting FluxDB Native Relationship Graphs Test (#6) ---\n";

    auto store = std::make_shared<ComponentStore>();
    auto pos_id = store->register_component("Position", sizeof(Position));
    auto hp_id  = store->register_component("Health", sizeof(Health));

    World world(store, std::make_shared<HistoryManager>(600));

    // Arquetipos de rol: unidad (pos) y torreta (pos, hp)
    constexpr RelationKind KIND_CHILD_OF = 0;
    constexpr RelationKind KIND_ATTACKS = 1;
    constexpr RelationKind KIND_SOCKET = 2; // payload: offset de montaje

    Entity tank = world.spawn();
    Entity turret = world.spawn();
    Entity pilot = world.spawn();

    Position p = {0.f, 0.f, 0.f};
    world.add_component(tank, pos_id, &p);
    world.add_component(turret, pos_id, &p);
    world.add_component(turret, hp_id, &p);

    // 1. Aristas forward/backward con payload
    world.add_relation(tank, KIND_CHILD_OF, pilot);
    RelationPayload offset;
    offset.set_as<float>(2.5f);
    world.add_relation(tank, KIND_SOCKET, turret, offset);
    world.add_relation(turret, KIND_ATTACKS, pilot);

    assert(world.has_relation(tank, KIND_CHILD_OF, pilot));
    assert(!world.has_relation(pilot, KIND_CHILD_OF, tank)); // dirigida

    RelationPayload out;
    assert(world.get_relation(tank, KIND_SOCKET, turret, out));
    assert(out.as<float>() == 2.5f); // payload survives roundtrip

    assert(world.outgoing_degree(tank, KIND_CHILD_OF) == 1);
    assert(world.incoming_degree(pilot, KIND_CHILD_OF) == 1);
    assert(world.incoming_degree(pilot, KIND_ATTACKS) == 1); // quien ataca al pilot

    std::cout << "1. Forward/backward indexing with typed payloads (OK)\n";

    // 2. Iteración forward y backward O(degree)
    std::vector<Entity> children;
    world.for_each_outgoing_relation(tank, KIND_CHILD_OF,
        [&](Entity dst, const RelationPayload&) { children.push_back(dst); });
    assert(children.size() == 1 && children[0] == pilot);

    std::vector<Entity> attackers;
    world.for_each_incoming_relation(pilot, KIND_ATTACKS,
        [&](Entity src, const RelationPayload&) { attackers.push_back(src); });
    assert(attackers.size() == 1 && attackers[0] == turret);

    std::cout << "2. Forward/backward iteration (O(degree)) (OK)\n";

    // 3. Re-add actualiza el payload, no duplica
    RelationPayload offset2;
    offset2.set_as<float>(9.0f);
    world.add_relation(tank, KIND_SOCKET, turret, offset2);
    assert(world.outgoing_degree(tank, KIND_SOCKET) == 1);
    assert(world.get_relation(tank, KIND_SOCKET, turret, out));
    assert(out.as<float>() == 9.0f);

    std::cout << "3. Re-add updates payload without duplicating edge (OK)\n";

    // 4. remove_relation limpia ambas direcciones
    assert(world.remove_relation(turret, KIND_ATTACKS, pilot));
    assert(!world.has_relation(turret, KIND_ATTACKS, pilot));
    assert(world.incoming_degree(pilot, KIND_ATTACKS) == 0);
    assert(!world.remove_relation(turret, KIND_ATTACKS, pilot)); // ya no existe

    std::cout << "4. remove_relation clears both directions (OK)\n";

    // 5. Versionado unificado (#4): los cambios de relación se sellan
    //    (fino por (src,kind) + grueso por kind)
    world.advance_tick(); // tick 1
    world.add_relation(turret, KIND_ATTACKS, pilot);
    assert(world.relation_last_write_tick(KIND_ATTACKS, turret) == 1);
    assert(world.relation_kind_changed_since(KIND_ATTACKS, 0));
    assert(!world.relation_kind_changed_since(KIND_ATTACKS, 1));
    assert(world.relation_last_write_tick(KIND_CHILD_OF, tank) == 0); // intacto

    world.advance_tick(); // tick 2
    world.remove_relation(turret, KIND_ATTACKS, pilot);
    assert(world.relation_last_write_tick(KIND_ATTACKS, turret) == 2);

    std::cout << "5. Relation changes stamped by the unified versioning (#4) (OK)\n";

    // 6. Despawn limpia todas las aristas del entity (como src y como dst)
    world.advance_tick(); // tick 3
    world.despawn(tank);
    assert(!world.has_relation(tank, KIND_CHILD_OF, pilot));
    assert(world.incoming_degree(pilot, KIND_CHILD_OF) == 0);
    assert(world.incoming_degree(tank, KIND_SOCKET) == 0); // turret ya no está "montada"
    assert(world.outgoing_degree(tank, KIND_SOCKET) == 0);

    // el grafo queda sin huecos hacia el entity eliminado
    std::vector<Entity> orphans;
    world.for_each_incoming_relation(tank, KIND_SOCKET,
        [&](Entity, const RelationPayload&) { orphans.push_back(1); });
    assert(orphans.empty());

    std::cout << "6. Despawn removes all edges (both directions) (OK)\n";

    // 7. clear_all resetea el grafo (rollback #8)
    world.advance_tick(); // tick 4
    Entity keeper = world.spawn();
    world.add_relation(keeper, KIND_CHILD_OF, pilot);
    world.clear_all();
    assert(!world.has_relation(keeper, KIND_CHILD_OF, pilot));
    assert(world.incoming_degree(pilot, KIND_CHILD_OF) == 0);
    assert(!world.relation_kind_changed_since(KIND_CHILD_OF, 0)); // rings reseteados

    std::cout << "7. clear_all resets the graph (OK)\n";

    // 8. Edges entre entidades inexistentes se ignoran
    world.add_relation(9999, KIND_CHILD_OF, 8888); // no existen
    assert(!world.has_relation(9999, KIND_CHILD_OF, 8888));
    assert(world.outgoing_degree(9999, KIND_CHILD_OF) == 0);

    std::cout << "8. Edges to unknown entities rejected (OK)\n";

    std::cout << "--- RELATIONSHIP GRAPHS TEST PASSED (#6) ---\n";
    return 0;
}
