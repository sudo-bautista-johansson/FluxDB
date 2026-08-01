#include "../core/headers/ecs.h"
#include "../core/headers/delta_set.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>
#include <cstdio>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::delta;

struct Health {
    int hp;
};

struct Position {
    float x, y, z;
};

// Trait compile-time (#7): Position se difunde cuantizado.
template <>
struct fluxdb::delta::DeltaCodec<Position> {
    static constexpr CodecID id() { return CodecID::QUANTIZED_FLOAT; }
};

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_codecs() {
    std::cout << "1. CodecRegistry / codecs...\n";

    CodecRegistry reg;
    CHECK(reg.get_codec(0) == CodecID::RAW);

    reg.set_codec(3, CodecID::QUANTIZED_FLOAT);
    CHECK(reg.get_codec(3) == CodecID::QUANTIZED_FLOAT);
    reg.set_codec(200, CodecID::RLE); // out of range: ignored
    CHECK(reg.get_codec(200) == CodecID::RAW);

    // RAW passthrough
    uint8_t raw_in[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    std::vector<uint8_t> enc;
    CHECK(reg.encode(0, raw_in, 4, enc));
    CHECK(enc.size() == 4 && std::memcmp(enc.data(), raw_in, 4) == 0);
    uint8_t raw_out[4] = {};
    CHECK(reg.decode(0, raw_out, 4, enc.data(), enc.size()));
    CHECK(std::memcmp(raw_out, raw_in, 4) == 0);

    // QUANTIZED_FLOAT roundtrip
    float pos_in[3] = {1.5f, -2.25f, 3000.0f};
    reg.set_codec(3, CodecID::QUANTIZED_FLOAT);
    enc.clear();
    CHECK(reg.encode(3, pos_in, sizeof(pos_in), enc));
    CHECK(enc.size() == 4 + 2 * 3); // scale + 3x int16
    float pos_out[3] = {};
    CHECK(reg.decode(3, pos_out, sizeof(pos_in), enc.data(), enc.size()));
    CHECK(std::fabs(pos_out[0] - 1.5f) < 0.1f);
    CHECK(std::fabs(pos_out[1] - -2.25f) < 0.1f);
    CHECK(std::fabs(pos_out[2] - 3000.0f) < 0.5f);

    // RLE roundtrip
    uint8_t rle_in[16];
    for (int i = 0; i < 10; ++i) rle_in[i] = 0x01;
    for (int i = 10; i < 16; ++i) rle_in[i] = 0x02;
    reg.set_codec(4, CodecID::RLE);
    enc.clear();
    CHECK(reg.encode(4, rle_in, 16, enc));
    uint8_t rle_out[16] = {};
    CHECK(reg.decode(4, rle_out, 16, enc.data(), enc.size()));
    CHECK(std::memcmp(rle_out, rle_in, 16) == 0);
}

static void test_delta_set_roundtrip() {
    std::cout << "2. DeltaSet serialize/deserialize roundtrip...\n";

    CodecRegistry reg;
    DeltaSet set1(7);
    Health h{60};
    set1.add_update(1, 0, &h, sizeof(h));
    set1.add_spawn(2);
    set1.add_despawn(3);

    std::vector<uint8_t> buf;
    set1.serialize(reg, buf);

    DeltaSet set2;
    CHECK(set2.deserialize(reg, buf.data(), buf.size()));
    CHECK(set2.record_count() == 3);
    CHECK(set2.base_tick() == 7);

    int updates = 0, spawns = 0, despawns = 0;
    set2.for_each_record([&](const DeltaRecord& r) {
        if (r.op == DeltaOp::UPDATE) {
            ++updates;
            CHECK(r.entity == 1);
            CHECK(r.comp_id == 0);
            CHECK(r.data.size() == sizeof(Health));
            CHECK(std::memcmp(r.data.data(), &h, sizeof(h)) == 0);
        } else if (r.op == DeltaOp::SPAWN) {
            ++spawns;
            CHECK(r.entity == 2);
        } else if (r.op == DeltaOp::DESPAWN) {
            ++despawns;
            CHECK(r.entity == 3);
        }
    });
    CHECK(updates == 1 && spawns == 1 && despawns == 1);

    // Corrupt payload must be rejected
    buf[0] = 0;
    DeltaSet bad;
    CHECK(!bad.deserialize(reg, buf.data(), buf.size()));
}

static void test_world_hooks() {
    std::cout << "3. World hooks (spawn_with_id / advance_to / events)...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    store->register_component("Health", sizeof(Health));

    Entity e = world.spawn_with_id(42);
    CHECK(e == 42);
    CHECK(world.spawn_with_id(42) == 42); // idempotente
    CHECK(world.spawn() == 43);           // next_entity_ sigue tras el ID explícito
    CHECK(world.spawn_with_id(10) == 10); // ID menor no rompe la secuencia

    CHECK(world.structural_events().size() == 3);
    CHECK(world.structural_events()[0].entity == 42 && world.structural_events()[0].spawned);

    world.despawn(10);
    CHECK(world.structural_events().size() == 4);
    CHECK(!world.structural_events()[3].spawned);

    world.prune_structural_events(0);
    CHECK(world.structural_events().empty());

    world.advance_to(50);
    CHECK(world.current_tick() == 50);
    world.advance_tick();
    CHECK(world.current_tick() == 51);

    CHECK(world.codec_registry().get_codec(0) == CodecID::RAW);
    world.set_codec(0, CodecID::RLE);
    CHECK(world.codec_registry().get_codec(0) == CodecID::RLE);
}

static void test_apply() {
    std::cout << "4. DeltaSet::apply rebuilds a world...\n";

    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(64));
    ComponentID hp = store->register_component("Health", sizeof(Health));
    world.set_codec(hp, CodecID::RLE); // los sinks aplican codecs al deserializar

    DeltaSet set(0);
    Health h_spawn{100};
    Health h_update{80};
    set.add_spawn(42);
    set.add_spawn(43, hp, &h_spawn, sizeof(h_spawn));
    set.add_update(42, hp, &h_update, sizeof(h_update));
    set.add_despawn(43);

    std::vector<uint8_t> buf;
    set.serialize(world.codec_registry(), buf);

    DeltaSet back;
    CHECK(back.deserialize(world.codec_registry(), buf.data(), buf.size()));
    back.apply(world);

    size_t sz = 0;
    const Health* h42 = static_cast<const Health*>(world.get_entity_component_data(42, hp, sz));
    CHECK(h42 != nullptr && h42->hp == 80);
    CHECK(world.get_entity_component_data(43, hp, sz) == nullptr); // despawned
}

static void test_replay() {
    std::cout << "5. Replay recorder -> player determinism...\n";
    const char* path = "test_replay.fxr";

    auto storeA = std::make_shared<ComponentStore>();
    World worldA(storeA, std::make_shared<HistoryManager>(64));
    ComponentID hp = storeA->register_component("Health", sizeof(Health));
    ComponentID pos = storeA->register_component("Position", sizeof(Position));
    worldA.set_codec(pos, CodecID::QUANTIZED_FLOAT);

    Entity e1 = worldA.spawn();
    Entity e2 = worldA.spawn();
    Health h{100};
    Position p{1.5f, -2.25f, 3000.0f};
    worldA.add_component(e1, hp, &h);
    worldA.add_component(e1, pos, &p);

    {
        ReplayRecorder rec(path);
        CHECK(rec.is_open());
        rec.begin_recording(worldA); // snapshot en tick 0

        worldA.advance_tick(); // tick 1
        h.hp = 80;
        worldA.add_component(e1, hp, &h);
        rec.record_tick(worldA);

        worldA.advance_tick(); // tick 2
        h.hp = 50;
        worldA.add_component(e1, hp, &h);
        Position p2{-10.0f, 0.5f, -99.9f};
        worldA.add_component(e2, pos, &p2);
        rec.record_tick(worldA);

        worldA.advance_tick(); // tick 3
        Health h3{75};
        worldA.add_component(e2, hp, &h3);
        rec.record_tick(worldA);
    } // close() parchea el conteo de ticks

    // Mundo B: reconstrucción determinista desde el archivo
    auto storeB = std::make_shared<ComponentStore>();
    World worldB(storeB, std::make_shared<HistoryManager>(64));

    ReplayPlayer player;
    CHECK(player.open(path));
    CHECK(player.start_tick() == 0);
    player.load_components(worldB);
    while (player.step(worldB)) {
    }
    CHECK(player.finished());

    size_t sz = 0;
    const Health* hB = static_cast<const Health*>(worldB.get_entity_component_data(e1, hp, sz));
    CHECK(hB != nullptr && hB->hp == 50);
    const Position* pB = static_cast<const Position*>(worldB.get_entity_component_data(e1, pos, sz));
    CHECK(pB != nullptr);
    CHECK(std::fabs(pB->x - 1.5f) < 0.1f);
    CHECK(std::fabs(pB->y - -2.25f) < 0.1f);
    CHECK(std::fabs(pB->z - 3000.0f) < 0.5f);
    const Health* h2B = static_cast<const Health*>(worldB.get_entity_component_data(e2, hp, sz));
    CHECK(h2B != nullptr && h2B->hp == 75);
    CHECK(worldB.get_entity_component_data(e2, pos, sz) != nullptr);
    CHECK(worldB.current_tick() == 3);
    // Solo spawns reales loguean eventos (los re-spawns idempotentes no):
    // e1 bare + e2 bare = 2
    CHECK(worldB.structural_events().size() == 2);

    // El archivo es autocontenido: codec de Position sobrevive al viaje
    CHECK(worldB.codec_registry().get_codec(pos) == CodecID::QUANTIZED_FLOAT);

    std::remove(path);
    std::cout << "   replay determinism OK (tick " << worldB.current_tick() << ")\n";
}

static void test_save_load() {
    std::cout << "6. save_incremental / load_from_replay...\n";
    const char* path = "test_save.fxr";

    auto storeA = std::make_shared<ComponentStore>();
    World worldA(storeA, std::make_shared<HistoryManager>(64));
    ComponentID hp = storeA->register_component("Health", sizeof(Health));

    Entity e = worldA.spawn();
    Health h{77};
    worldA.add_component(e, hp, &h);

    CHECK(worldA.save_incremental(path));

    auto storeB = std::make_shared<ComponentStore>();
    World worldB(storeB, std::make_shared<HistoryManager>(64));
    CHECK(worldB.load_from_replay(path));

    size_t sz = 0;
    const Health* hB = static_cast<const Health*>(worldB.get_entity_component_data(e, hp, sz));
    CHECK(hB != nullptr && hB->hp == 77);

    std::remove(path);
}

static void test_bitpack() {
    std::cout << "7. BITPACK codec + DeltaCodec<T> trait...\n";

    // BITPACK: arrays de enteros empaquetados al ancho de bits mínimo.
    CodecRegistry reg;
    reg.set_codec(5, CodecID::BITPACK);
    uint32_t vals[8] = {1, 2, 3, 255, 0, 0, 7, 300}; // max 300 -> 9 bits
    std::vector<uint8_t> enc;
    CHECK(reg.encode(5, vals, sizeof(vals), enc));
    CHECK(enc.size() < sizeof(vals)); // 6 + ceil(72/8)=9 -> 15 < 32 bytes
    uint32_t out[8] = {};
    CHECK(reg.decode(5, out, sizeof(vals), enc.data(), enc.size()));
    CHECK(std::memcmp(out, vals, sizeof(vals)) == 0);

    // Todos ceros -> bit_width 0, sin datos empaquetados
    uint32_t zeros[4] = {};
    enc.clear();
    CHECK(reg.encode(5, zeros, sizeof(zeros), enc));
    uint32_t zout[4] = {9, 9, 9, 9};
    CHECK(reg.decode(5, zout, sizeof(zeros), enc.data(), enc.size()));
    CHECK(zout[0] == 0 && zout[3] == 0);

    // u16 con valores grandes (ancho 16: sin ganancia pero correcto)
    uint16_t big[3] = {60000, 1, 65535};
    enc.clear();
    CHECK(reg.encode(5, big, sizeof(big), enc));
    uint16_t bout[3] = {};
    CHECK(reg.decode(5, bout, sizeof(big), enc.data(), enc.size()));
    CHECK(bout[0] == 60000 && bout[1] == 1 && bout[2] == 65535);

    // Trait DeltaCodec<T> (#7): set_codec<T> usa la estrategia del tipo.
    auto store = std::make_shared<ComponentStore>();
    World world(store, std::make_shared<HistoryManager>(16));
    ComponentID pos = store->register_component("Position", sizeof(Position));
    CHECK(world.codec_registry().get_codec(pos) == CodecID::RAW);
    world.set_codec<Position>(pos);
    CHECK(world.codec_registry().get_codec(pos) == CodecID::QUANTIZED_FLOAT);

    // y el payload resultante viaja comprimido por el trait
    Entity e = world.spawn();
    Position p{123.0f, -45.0f, 0.0f};
    world.add_component(e, pos, &p);
    DeltaSet d = capture_world_snapshot(world);
    std::vector<uint8_t> buf;
    d.serialize(world.codec_registry(), buf);
    size_t raw_estimate = 29 + 2 /*tabla de codecs*/ + 2 * (1 + 4 + 1 + 4 + 4) + sizeof(Position); // sin codec
    CHECK(buf.size() < raw_estimate); // el trait comprimió el payload
}

static void test_relation_events() {
    std::cout << "8. Relation events replicable en DeltaSet (#6)...\n";
    constexpr RelationKind KIND_CHILD_OF = 0;

    auto storeA = std::make_shared<ComponentStore>();
    World worldA(storeA, std::make_shared<HistoryManager>(64));
    Entity a = worldA.spawn(); // 0
    Entity b = worldA.spawn(); // 1
    Entity c = worldA.spawn(); // 2

    worldA.advance_tick(); // tick 1
    worldA.add_relation(a, KIND_CHILD_OF, b);
    worldA.add_relation(a, KIND_CHILD_OF, c);
    worldA.advance_tick(); // tick 2
    worldA.remove_relation(a, KIND_CHILD_OF, b);

    // Snapshots llevan las aristas vivas del estado final
    DeltaSet snap = capture_world_snapshot(worldA);
    auto storeB = std::make_shared<ComponentStore>();
    World worldB(storeB, std::make_shared<HistoryManager>(64));
    snap.apply(worldB);
    CHECK(worldB.has_relation(a, KIND_CHILD_OF, c));
    CHECK(!worldB.has_relation(a, KIND_CHILD_OF, b)); // removida en tick 2

    // Diffs de tick llevan adds y removals (tombstones) como eventos nativos
    DeltaSet d = capture_tick_delta(worldA, 1); // solo el cambio del tick 2
    std::vector<uint8_t> buf;
    d.serialize(worldA.codec_registry(), buf);
    DeltaSet wire;
    CHECK(wire.deserialize(worldA.codec_registry(), buf.data(), buf.size()));
    int rel_events = 0;
    wire.for_each_record([&](const DeltaRecord& r) {
        if (r.op == DeltaOp::RELATION) ++rel_events;
    });
    CHECK(rel_events == 2); // remove(a,b) + re-add(a,c)
    wire.apply(worldB);
    CHECK(worldB.has_relation(a, KIND_CHILD_OF, c));
    CHECK(!worldB.has_relation(a, KIND_CHILD_OF, b)); // el removal se propagó

    // Limpieza de tombstones: podar antes del tick 2 deja de emitir removals
    worldA.prune_structural_events(2); // poda eventos y tumbas <= 2
    DeltaSet d2 = capture_tick_delta(worldA, 0);
    int rel_after = 0;
    d2.for_each_record([&](const DeltaRecord& r) {
        if (r.op == DeltaOp::RELATION) {
            CHECK(r.data[0] == 0); // solo adds (el removal ya se podó)
            ++rel_after;
        }
    });
    CHECK(rel_after == 1); // re-add del estado vivo (a -> c) únicamente
}

static void test_compact_save() {
    std::cout << "9. compact_save (delta folding)...\n";
    const char* path = "test_compact.fxr";

    auto storeA = std::make_shared<ComponentStore>();
    World worldA(storeA, std::make_shared<HistoryManager>(64));
    ComponentID hp = storeA->register_component("Health", sizeof(Health));

    ReplayRecorder rec;
    CHECK(rec.open(path));
    rec.begin_recording(worldA);

    Entity e1 = worldA.spawn(); // tick 0
    Entity e2 = worldA.spawn();
    Health h{100};
    worldA.add_component(e1, hp, &h);
    h.hp = 50;
    worldA.add_component(e2, hp, &h);
    rec.record_tick(worldA);

    worldA.advance_tick(); // tick 1
    h.hp = 80;
    worldA.add_component(e1, hp, &h);
    rec.record_tick(worldA);

    worldA.advance_tick(); // tick 2
    worldA.despawn(e2);
    h.hp = 10;
    worldA.add_component(e1, hp, &h);
    rec.record_tick(worldA);
    rec.close();

    // Folding: la cadena de deltas se pliega en el snapshot base
    CHECK(worldA.compact_save(path));

    // El archivo compacto carga el estado FINAL sin la cadena
    auto storeB = std::make_shared<ComponentStore>();
    World worldB(storeB, std::make_shared<HistoryManager>(64));
    CHECK(worldB.load_from_replay(path));

    size_t sz = 0;
    const Health* hB = static_cast<const Health*>(worldB.get_entity_component_data(e1, hp, sz));
    CHECK(hB != nullptr && hB->hp == 10); // last-write-wins
    CHECK(worldB.get_entity_component_data(e2, hp, sz) == nullptr); // despawn plegado
    CHECK(worldB.current_tick() == 2); // el compacto lleva el tick final

    std::remove(path);
}

static void test_incremental_save_workflow() {
    std::cout << "10. save_incremental chain (base + delta incremental)...\n";

    // Fase 1: crear mundo inicial, guardar como base.
    auto store1 = std::make_shared<ComponentStore>();
    World world1(store1, std::make_shared<HistoryManager>(64));
    ComponentID hp = store1->register_component("Health", sizeof(Health));

    Entity e = world1.spawn();
    Health h100{100};
    world1.add_component(e, hp, &h100);
    // Añadir otro componente que cambie
    ComponentID mana = store1->register_component("Mana", sizeof(int));
    int m1 = 50;
    world1.add_component(e, mana, &m1);

    CHECK(world1.save_incremental("test_base.fxr"));

    // Fase 2: mutar el mundo, guardar incremental desde la base.
    world1.advance_tick();
    int m2 = 75;
    world1.add_component(e, mana, &m2);
    Health h80{80};
    world1.add_component(e, hp, &h80); // daño

    CHECK(world1.save_incremental("test_incr.fxr", "test_base.fxr"));

    // Fase 3: cargar el archivo incremental y verificar el estado final.
    auto store2 = std::make_shared<ComponentStore>();
    World world2(store2, std::make_shared<HistoryManager>(64));
    CHECK(world2.load_from_replay("test_incr.fxr"));

    size_t sz = 0;
    const Health* h = static_cast<const Health*>(world2.get_entity_component_data(e, hp, sz));
    CHECK(h != nullptr && h->hp == 80);
    const int* mp = static_cast<const int*>(world2.get_entity_component_data(e, mana, sz));
    CHECK(mp != nullptr && *mp == 75);

    // Fase 4: guardar otra vez desde el incremental (cadena de 3).
    world2.advance_tick();
    int m3 = 100;
    world2.add_component(e, mana, &m3);

    CHECK(world2.save_incremental("test_chain.fxr", "test_incr.fxr"));

    auto store3 = std::make_shared<ComponentStore>();
    World world3(store3, std::make_shared<HistoryManager>(64));
    CHECK(world3.load_from_replay("test_chain.fxr"));

    const int* mp3 = static_cast<const int*>(world3.get_entity_component_data(e, mana, sz));
    CHECK(mp3 != nullptr && *mp3 == 100);
    const Health* h3 = static_cast<const Health*>(world3.get_entity_component_data(e, hp, sz));
    CHECK(h3 != nullptr && h3->hp == 80);

    std::remove("test_base.fxr");
    std::remove("test_incr.fxr");
    std::remove("test_chain.fxr");
}

int main() {
    std::cout << "--- Starting FluxDB Unified Delta Engine Test (#7) ---\n";

    test_codecs();
    test_delta_set_roundtrip();
    test_world_hooks();
    test_apply();
    test_replay();
    test_save_load();
    test_bitpack();
    test_relation_events();
    test_compact_save();
    test_incremental_save_workflow();

    std::cout << "--- DELTA SET TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}
