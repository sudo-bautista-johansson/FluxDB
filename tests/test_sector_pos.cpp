// FluxDB — Feature #19: Infinite World Origin Rebasing
// Posiciones sector-relative nativas (SectorPos): descomposición exacta
// world→(sector, offset), distancia sin jitter a 1e9+, grid espacial keyed
// por sectores, integración World/pubsub y compresión de red.
#include "../core/headers/ecs.h"
#include "../core/headers/sector_pos.h"
#include "../core/headers/pubsub.h"
#include "../core/headers/delta_set.h"
#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>

using namespace fluxdb;
using namespace fluxdb::ecs;
using namespace fluxdb::query;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_decomposition() {
    std::cout << "1. Descomposición world → (sector, offset)...\n";

    // Roundtrip exacto en rangos normales
    SectorPos p = SectorPos::from_world(1234.5f, -999.25f, 0.0f);
    CHECK(p.sx == 1 && p.sy == -1 && p.sz == 0);
    CHECK(std::fabs(p.ox - 210.5f) < 1e-4f);
    CHECK(std::fabs(p.oy - 24.75f) < 1e-4f);

    float wx, wy, wz;
    p.to_world(wx, wy, wz);
    CHECK(std::fabs(wx - 1234.5f) < 1e-4f);
    CHECK(std::fabs(wy + 999.25f) < 1e-4f);

    // Floor correcto para negativos: -1.0 → sector 0, offset -1 (centrado)
    SectorPos n = SectorPos::from_world(-1.0f, 0.0f, 0.0f);
    CHECK(n.sx == 0);
    CHECK(std::fabs(n.ox + 1.0f) < 1e-4f);
    n.to_world(wx, wy, wz);
    CHECK(std::fabs(wx + 1.0f) < 1e-4f);

    // Mundos "infinitos": roundtrip a 1e9 (float ulp a esa magnitud ≈ 64)
    SectorPos big = SectorPos::from_world(1000000000.0f, -1000000000.0f, 123456789.0f);
    CHECK(big.sx == 976563 && big.sy == -976562); // centrado: (x±512)/1024
    CHECK(big.ox == -512.0f && big.oy == -512.0f);
    big.to_world(wx, wy, wz);
    CHECK(std::fabs(wx - 1000000000.0f) <= 128.0f);
    CHECK(std::fabs(wy + 1000000000.0f) <= 128.0f);
    CHECK(std::fabs(wz - 123456789.0f) <= 128.0f);

    // Offsets siempre en [-512, 512)
    for (float v : {-9000000.0f, -9000001.0f, 3.5f, 9000000.0f}) {
        SectorPos q = SectorPos::from_world(v, 0.0f, 0.0f);
        CHECK(q.ox >= -SECTOR_HALF && q.ox < SECTOR_HALF);
    }
}

static void test_far_origin_precision() {
    std::cout << "2. Precisión a 1e9 (el headline: sin jitter)...\n";

    // Dos posiciones a mil millones de unidades separadas por 64 unidades
    // (el ulp de float a 1e9: 1000000001.0f ni siquiera existe como float —
    // el mínimo incremento representable es 64).
    SectorPos a = SectorPos::from_world(1000000000.0f, 0.0f, 0.0f);
    SectorPos b = SectorPos::from_world(1000000064.0f, 0.0f, 0.0f);

    float wa[3], wb[3];
    a.to_world(wa);
    b.to_world(wb);
    bool jittered_world = (wb[0] - wa[0]) == 1.0f; // float de mundo NO puede
    CHECK(!jittered_world);                        // ... y no importa: no se usa

    float d = a.distance_to(b);
    CHECK(d == 64.0f); // EXACTO (el mundo a 1e9 ni siquiera puede distinguir 1.0)

    // Distancia a través de frontera de sector: 511.5 ↔ 512.5 → 1.0 exacto
    SectorPos c = SectorPos::from_world(511.5f, 0.0f, 0.0f);
    SectorPos dd = SectorPos::from_world(512.5f, 0.0f, 0.0f);
    CHECK(c.distance_to(dd) == 1.0f);
    CHECK(!same_sector(c, dd));

    // Coordenadas relativas al sector de referencia (query-time): exactas
    // cerca del centro incluso a 1e9 de mundo. b vive en el mismo sector
    // que a (offset -448 vs -512): relativo = -448, delta = 64 exacto.
    float rx, ry, rz, ax2, ay2, az2;
    b.to_world_relative(a.sx, a.sy, a.sz, rx, ry, rz);
    a.to_world_relative(a.sx, a.sy, a.sz, ax2, ay2, az2);
    CHECK(rx == -448.0f && ax2 == -512.0f);
    CHECK(rx - ax2 == 64.0f);

    // delta_to (para el codec de red)
    int32_t dsec[3]; float doff[3];
    a.delta_to(b, dsec, doff);
    CHECK(dsec[0] == 0 && doff[0] == 64.0f);
}

static void test_sector_grid() {
    std::cout << "3. SpatialSectorGrid (keyed por sectores)...\n";

    SpatialSectorGrid grid;

    // Entidades repartidas entre sectores (coordenadas float exactas)
    grid.update(1, SectorPos::from_world(10.0f, 0.0f, 0.0f));
    grid.update(2, SectorPos::from_world(511.5f, 0.0f, 0.0f));  // mismo sector, ox 511.5
    grid.update(3, SectorPos::from_world(513.0f, 0.0f, 0.0f));  // cruza la frontera (sector +1)
    grid.update(4, SectorPos::from_world(0.0f, 9000.0f, 0.0f)); // sector +8
    grid.update(5, SectorPos::from_world(-999999999.0f, 999999999.0f, 999999999.0f)); // lejos

    // Query de radio 600 en el origen local: entidades 1,2,3 (no 4 ni 5)
    SectorPos center = SectorPos::from_world(10.0f, 0.0f, 0.0f);
    std::vector<std::pair<Entity, SectorPos>> hits;
    grid.query_range(center, 600.0f, hits);
    CHECK(hits.size() == 3);
    bool saw1 = false, saw2 = false, saw3 = false;
    for (const auto& [e, p] : hits) {
        if (e == 1) saw1 = true;
        if (e == 2) saw2 = true;
        if (e == 3) saw3 = true;
    }
    CHECK(saw1 && saw2 && saw3);

    // Movimiento entre sectores: 3 se va a otro sector → fuera del radio
    grid.update(3, SectorPos::from_world(0.0f, 5000.0f, 0.0f));
    hits.clear();
    grid.query_range(center, 600.0f, hits);
    CHECK(hits.size() == 2);

    // remove
    grid.remove(2);
    hits.clear();
    grid.query_range(center, 600.0f, hits);
    CHECK(hits.size() == 1);
    CHECK(grid.find(2) == nullptr);

    // Radio que cruza fronteras de sector: 500 ↔ 1500 ↔ 2000
    SpatialSectorGrid g2;
    g2.update(1, SectorPos::from_world(500.0f, 0.0f, 0.0f));
    g2.update(2, SectorPos::from_world(1500.0f, 0.0f, 0.0f)); // sector +1
    g2.update(3, SectorPos::from_world(2000.0f, 0.0f, 0.0f)); // sector +1, más lejos
    hits.clear();
    g2.query_range(SectorPos::from_world(1000.0f, 0.0f, 0.0f), 520.0f, hits);
    CHECK(hits.size() == 2); // 1 y 2; 3 queda a 1000
}

static void test_world_pubsub_sector() {
    std::cout << "4. Integración World + pub/sub a 1e9 de mundo...\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(SectorPos));
    world.set_position_component_id(pos_id);
    CHECK(world.sector_positions()); // detectado por tamaño

    // AOI del suscriptor en mundo a 1e9 (radio 1200: permite cruzar la
    // frontera del sector 976563 → 976564 a offsets representables)
    InterestVolume aoi;
    aoi.shape = VolumeShape::SPHERE;
    aoi.cx = 1000000000.0f; aoi.cy = 0.0f; aoi.cz = 0.0f;
    aoi.r = 1200.0f;
    uint32_t sub = pubsub->subscribe_volume("", aoi, nullptr);

    // Dentro del AOI (a 10m del centro) y fuera (a ~9km, OTRO sector)
    Entity inside = world.spawn();
    Entity outside = world.spawn();
    SectorPos pin = SectorPos::from_world(1000000010.0f, 0.0f, 0.0f);
    SectorPos pout = SectorPos::from_world(1000009000.0f, 0.0f, 0.0f);
    world.add_component(inside, pos_id, &pin);
    world.add_component(outside, pos_id, &pout);

    // sector_position del world (grid sectorial)
    const SectorPos* got = world.sector_position(inside);
    CHECK(got != nullptr);
    CHECK(*got == pin);
    CHECK(world.sector_position(outside) != nullptr);

    world.flush_interest_events();

    // Relevancia: inside dentro del AOI, outside NO (aunque el índice de
    // mundo legacy -10km..10km jamás habría visto a 1e9).
    const std::unordered_set<uint32_t>& rel = pubsub->relevant_entities(sub);
    CHECK(rel.count(inside) == 1);
    CHECK(rel.count(outside) == 0);

    // Movimiento al SECTOR SIGUIENTE (sector 976564, ox=-384 → dist 1152):
    // sigue dentro del AOI por distancia sector-exacta (sin jitter de float)
    SectorPos pnear = SectorPos::from_world(1000001152.0f, 0.0f, 0.0f);
    CHECK(pnear.sx == 976564); // de verdad cruzó la frontera
world.add_component(inside, pos_id, &pnear);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(sub).count(inside) == 1);
    CHECK(world.sector_position(inside)->ox == pnear.ox);
}

static void test_network_compression() {
    std::cout << "5. Compresión de red sector-relative (SECTOR_POS codec)...\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(SectorPos));
    world.set_position_component_id(pos_id);
    world.set_codec(pos_id, delta::CodecID::SECTOR_POS);

    // Codec directo: 22 bytes vs 24 RAW; roundtrip sub-centímetro
    SectorPos p = SectorPos::from_world(1000000000.5f, 123.456f, -78.9f);
    delta::CodecRegistry reg;
    reg.set_codec(pos_id, delta::CodecID::SECTOR_POS);
    std::vector<uint8_t> enc;
    CHECK(reg.encode(pos_id, &p, sizeof(p), enc));
    CHECK(enc.size() < sizeof(SectorPos)); // más pequeño que RAW
    CHECK(enc.size() == 22);
    SectorPos back{};
    CHECK(reg.decode(pos_id, &back, sizeof(back), enc.data(), enc.size()));
    CHECK(back.sx == p.sx && back.sy == p.sy && back.sz == p.sz);
    CHECK(std::fabs(back.ox - p.ox) < 0.02f);
    CHECK(std::fabs(back.oy - p.oy) < 0.02f);
    CHECK(std::fabs(back.oz - p.oz) < 0.02f);

    // Roundtrip completo por DeltaSet: update → serialize → apply
    Entity e = world.spawn();
    world.add_component(e, pos_id, &p);

    // Sink de red (#7): el delta de un suscriptor con código sectorial
    InterestVolume aoi;
    aoi.shape = VolumeShape::SPHERE;
    aoi.cx = 1000000000.0f; aoi.cy = 0.0f; aoi.cz = 0.0f;
    aoi.r = 1000.0f;
    uint32_t sub = pubsub->subscribe_volume("", aoi, nullptr);
    world.advance_tick(); // sella el write (LOD exige last_write > 0)
    SectorPos p2 = SectorPos::from_world(1000000010.0f, 10.0f, 0.0f);
    world.add_component(e, pos_id, &p2);
    world.set_component_lod(pos_id, {{5000.0f, 1, 0.0f}});
    world.flush_interest_events();

    delta::DeltaSet d = world.build_subscriber_delta(sub, 0);
    bool saw_update = false;
    SectorPos received{};
    d.for_each_record([&](const delta::DeltaRecord& r) {
        if (r.op == delta::DeltaOp::UPDATE && r.comp_id == pos_id) {
            saw_update = true;
            std::memcpy(&received, r.data.data(), sizeof(received));
        }
    });
    CHECK(saw_update);
    CHECK(received.sx == p2.sx && received.sy == p2.sy);
    CHECK(std::fabs(received.ox - p2.ox) < 0.02f);
}

static void test_legacy_backward_compat() {
    std::cout << "6. Backward compat: posición float clásica...\n";

    auto store = std::make_shared<ComponentStore>();
    auto pubsub = std::make_shared<SubscriptionManager>();
    World world(store, nullptr, pubsub);

    ComponentID pos_id = store->register_component("Position", sizeof(float) * 3);
    world.set_position_component_id(pos_id);
    CHECK(!world.sector_positions()); // NO es sector: modo legacy

    InterestVolume aoi;
    aoi.shape = VolumeShape::SPHERE;
    aoi.cx = 0; aoi.cy = 0; aoi.cz = 0;
    aoi.r = 100.0f;
    uint32_t sub = pubsub->subscribe_volume("", aoi, nullptr);

    Entity e = world.spawn();
    float pos[3] = {10.0f, 0.0f, 0.0f};
    world.add_component(e, pos_id, pos);
    world.flush_interest_events();
    CHECK(pubsub->relevant_entities(sub).count(e) == 1);
    CHECK(world.sector_position(e) == nullptr); // sin grid sectorial
}

int main() {
    std::cout << std::unitbuf;
    std::cout << "--- Starting FluxDB Infinite World Origin Rebasing Test (#19) ---\n";
    test_decomposition();
    test_far_origin_precision();
    test_sector_grid();
    test_world_pubsub_sector();
    test_network_compression();
    test_legacy_backward_compat();
    std::cout << "--- SECTOR POS TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}
