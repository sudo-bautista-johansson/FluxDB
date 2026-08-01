#include "../core/headers/ecs.h"
#include "../core/headers/network.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include <algorithm>

// ─────────────────────────────────────────
//  Roadmap #4: Temporal Component Versioning
//  Change Ring Buffers (coarse per-array) + per-entity fine ticks
// ─────────────────────────────────────────

using namespace fluxdb::ecs;

struct Position { float x, y, z; };
struct Health   { int hp; };

static size_t count_changed(World& world, ComponentID comp_id, uint64_t since_tick) {
    size_t count = 0;
    world.for_each_changed(comp_id, since_tick, [&](Entity, size_t, uint32_t) { ++count; });
    return count;
}

int main() {
    std::cout << "--- Starting FluxDB Temporal Component Versioning Test (#4) ---\n";

    auto store = std::make_shared<ComponentStore>();
    auto pos_id = store->register_component("Position", sizeof(Position));
    auto hp_id  = store->register_component("Health", sizeof(Health));

    // Con HistoryManager adjunto: versioning y time-travel deben sellar el mismo tick
    World world(store, std::make_shared<HistoryManager>(600));

    Entity e1 = world.spawn();
    Entity e2 = world.spawn();
    Entity e3 = world.spawn();

    Position p = {0.f, 0.f, 0.f};
    Health h = {100};

    // Tick 0: estado inicial (stamped en tick 0)
    world.add_component(e1, pos_id, &p);
    world.add_component(e1, hp_id, &h);
    world.add_component(e2, pos_id, &p);
    world.add_component(e2, hp_id, &h);

    // 1. Nada cambió después del tick 0
    assert(count_changed(world, pos_id, 0) == 0);
    assert(count_changed(world, hp_id, 0) == 0);
    assert(!world.entity_changed_since(e1, pos_id, 0));

    std::cout << "1. No changes since tick 0 -> empty changed set (OK)\n";

    // 2. Tick 1: e1 se mueve, e2 no
    world.advance_tick(); // tick 1
    p.x = 10.f;
    world.add_component(e1, pos_id, &p);

    assert(count_changed(world, pos_id, 0) == 1);   // solo e1
    assert(count_changed(world, pos_id, 1) == 0);   // nada después del tick 1
    assert(count_changed(world, hp_id, 0) == 0);    // health intacto
    assert(world.entity_changed_since(e1, pos_id, 0));
    assert(!world.entity_changed_since(e2, pos_id, 0));
    assert(world.entity_last_write_tick(e1, pos_id) == 1);
    assert(world.entity_last_write_tick(e2, pos_id) == 0);

    std::cout << "2. Tick 1 change tracked per-entity (OK)\n";

    // 3. Tick 2: e2 recibe daño; e1 muta health también
    world.advance_tick(); // tick 2
    h.hp = 80;
    world.add_component(e2, hp_id, &h);
    h.hp = 50;
    world.add_component(e1, hp_id, &h);

    assert(count_changed(world, hp_id, 1) == 2);    // e1 y e2, no e3
    assert(count_changed(world, hp_id, 0) == 2);
    assert(count_changed(world, pos_id, 0) == 1);   // e1 sigue siendo el único con pos dirty
    assert(world.entity_last_write_tick(e2, hp_id) == 2);

    std::cout << "3. Multiple entities, multiple components tracked independently (OK)\n";

    // 4. Filtro grueso O(1): un arquetipo sin writes se salta entero
    // e3 no tiene componentes: nunca debe aparecer
    assert(count_changed(world, pos_id, 0) == 1);
    assert(world.entity_last_write_tick(e3, pos_id) == 0);

    std::cout << "4. Unmodified archetype/entities skipped (OK)\n";

    // 5. add_component con componente NUEVO: se escribe y se sella (bug fix)
    world.advance_tick(); // tick 3
    Position p3 = {99.f, 0.f, 0.f};
    world.add_component(e3, pos_id, &p3);

    size_t size = 0;
    const Position* read = (const Position*)world.get_entity_component_data(e3, pos_id, size);
    assert(size == sizeof(Position));
    assert(read && read->x == 99.f);   // el data debe persistir (antes se perdía)
    assert(world.entity_last_write_tick(e3, pos_id) == 3);
    assert(count_changed(world, pos_id, 2) == 1);   // e3

    std::cout << "5. New-component add writes data + stamps version (bug fix, OK)\n";

    // 6. Ring grueso: el arquetipo vacío no genera falsos positivos en queries
    assert(count_changed(world, hp_id, 2) == 0);    // nada de health después del tick 2
    assert(count_changed(world, hp_id, 1) == 2);    // e1+e2 modificados en tick 2

    std::cout << "6. Coarse ring filter per component array (OK)\n";

    // 7. El tick de versioning y el de history están en sync vía World::advance_tick
    assert(world.current_tick() == 3);
    assert(world.get_history()->get_current_tick() == 3);

    std::cout << "7. VersionTracker/History tick sync (OK)\n";

    // 8. Integración: DeltaCompression usa el versionado (#7 backbone).
    // El payload ahora es un DeltaSet unificado (#7): header [magic(4) version(1)
    // base(8) end(8) num_codecs(4)] y num_records en el offset 25.
    fluxdb::network::DeltaCompression delta(&world);
    std::vector<uint8_t> payload;
    delta.generate_delta_payload(0, payload);

    uint32_t num_records = 0;
    std::memcpy(&num_records, payload.data() + 25, sizeof(uint32_t));
    assert(num_records == 4); // e1 pos(1) + e1 hp(2) + e2 hp(2) + e3 pos(3)

    std::cout << "8. DeltaCompression powered by versioning (" << num_records << " dirty records) (OK)\n";

    // ── Chunk-granularity (#4): >256 filas cruzan límites de chunk ──
    // Poblar 400 entidades con Position: la fila == id (orden de spawn).
    std::vector<Entity> batch;
    for (int i = 0; i < 400; ++i) batch.push_back(world.spawn());
    for (Entity e : batch) world.add_component(e, pos_id, &p); // tick 3

    const auto& archs = world.get_archetypes();
    ArchetypeSignature sig;
    sig.set(pos_id);
    auto arch_it = archs.find(sig.to_ullong());
    assert(arch_it != archs.end());
    const Archetype* pos_arch = arch_it->second.get();
    assert(pos_arch->chunk_count(pos_id) == 2); // 403 filas → 2 chunks de 256

    // 9. Write solo en chunk 0: el chunk 1 se salta entero (O(1) por chunk)
    world.advance_tick(); // tick 4
    Position p200 = {5.f, 0.f, 0.f};
    world.add_component(200, pos_id, &p200);
    assert(pos_arch->chunk_has_writes_since(pos_id, 0, 3));
    assert(!pos_arch->chunk_has_writes_since(pos_id, 1, 3));
    assert(count_changed(world, pos_id, 3) == 1); // solo entity 200

    std::cout << "9. Chunk-granular skip: write in chunk 0, chunk 1 clean (OK)\n";

    // 10. Write en chunk 1: se reporta sin tocar el resto
    world.advance_tick(); // tick 5
    Position p400 = {7.f, 0.f, 0.f};
    world.add_component(400, pos_id, &p400);
    assert(pos_arch->chunk_has_writes_since(pos_id, 1, 4));
    assert(count_changed(world, pos_id, 4) == 1); // solo entity 400

    std::cout << "10. Chunk 1 dirtied independently (OK)\n";

    // 11. Crecimiento estructural: 300 más → nace el chunk 2
    world.advance_tick(); // tick 6
    std::vector<Entity> batch2;
    for (int i = 0; i < 300; ++i) batch2.push_back(world.spawn());
    for (Entity e : batch2) world.add_component(e, pos_id, &p400);
    assert(pos_arch->chunk_count(pos_id) == 3); // 703 filas → 3 chunks
    assert(count_changed(world, pos_id, 5) == 300); // solo las nuevas

    std::cout << "11. Structural growth spawns new chunk (OK)\n";

    // 12. Swap-and-pop con despawn sin tick: el chunk destino del movido
    //     se marca con SU tick, para no perderlo en for_each_changed (fix).
    world.advance_tick(); // tick 7
    Position p702 = {9.f, 0.f, 0.f};
    world.add_component(702, pos_id, &p702);
    world.advance_tick(); // tick 8
    world.despawn(3); // swap de la fila 699 (entity 702, tick 7) a la fila 0 (chunk 0)
    assert(pos_arch->chunk_has_writes_since(pos_id, 0, 6));
    assert(count_changed(world, pos_id, 6) == 1); // la entidad movida
    assert(world.entity_changed_since(702, pos_id, 6));

    std::cout << "12. Swap-and-pop marks destination chunk (fix, OK)\n";

    std::cout << "--- TEMPORAL COMPONENT VERSIONING TEST PASSED (#4) ---\n";
    return 0;
}
