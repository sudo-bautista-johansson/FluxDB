// FluxDB — Feature #17: Streaming NavMesh Tied to World Streaming
// Tiles de navegación indexados por coords de chunk (#20); pathfinding
// chequea residencia y pide carga just-in-time.
#include "../core/headers/navmesh.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb::nav;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_coord_convention() {
    std::cout << "1. Convención de coordenadas compartida con #20...\n";

    NavMeshStreamer nm;
    NavTileCoord c0 = nm.coord_for(0, 0, 0);
    CHECK(c0.cx == 0 && c0.cy == 0 && c0.cz == 0);

    NavTileCoord c1 = nm.coord_for(2048.0f, 0, 0);
    CHECK(c1.cx == 2 && c1.cy == 0 && c1.cz == 0);

    NavTileCoord cneg = nm.coord_for(-1500.0f, 0, 0);
    CHECK(cneg.cx == -2); // floor(-1.46) = -2

    NavTileCoord same1 = nm.coord_for(1024.0f, 512.0f, 0);
    NavTileCoord same2 = nm.coord_for(2047.0f, 1023.0f, 512);
    CHECK(same1.cx == 1 && same2.cy == 0);
}

static void test_residency_and_load_requests() {
    std::cout << "2. Residencia y peticiones de carga JIT...\n";

    NavMeshStreamer nm;
    NavTileCoord c = nm.coord_for(100, 0, 0);

    CHECK(!nm.is_resident(c));
    CHECK(nm.resident_count() == 0);

    // request_load de un tile no residente → pendiente.
    CHECK(!nm.request_load(c));
    CHECK(nm.pending().size() == 1);

    // El streaming lo materializa.
    NavTile tile;
    tile.coord = c;
    NavNode n; n.id = 1; n.x = 10; n.y = 0; n.z = 0;
    tile.nodes.push_back(n);
    nm.provide_tile(tile);

    CHECK(nm.is_resident(c));
    CHECK(nm.resident_count() == 1);
    CHECK(nm.pending().size() == 0);

    // Ya residente → request_load devuelve true, sin nueva pendiente.
    CHECK(nm.request_load(c));
    CHECK(nm.pending().size() == 0);

    // get() devuelve el tile residente.
    NavTile* got = nm.get(c);
    CHECK(got != nullptr);
    CHECK(got->nodes.size() == 1);

    // unload.
    nm.unload(c);
    CHECK(!nm.is_resident(c));
    CHECK(nm.get(c) == nullptr);
}

static void test_pathfinding_streams_missing_tiles() {
    std::cout << "3. Pathfinding: carga JIT de tiles faltantes...\n";

    NavMeshStreamer nm;

    // Tile del origen y del destino residentes.
    NavTileCoord t0 = nm.coord_for(0, 0, 0);
    NavTile tile0; tile0.coord = t0;
    nm.provide_tile(tile0);

    NavTileCoord t4 = nm.coord_for(4096, 0, 0); // tile 4
    NavTile tile4; tile4.coord = t4;
    nm.provide_tile(tile4);

    NavPathfinder pf(nm, 64.0f);

    // Destino dentro del mismo tile → camino directo.
    std::vector<NavPathPoint> path;
    bool streaming = false;
    CHECK(pf.find_path(0, 0, 0, 100, 0, 0, path, streaming));
    CHECK(!streaming);
    CHECK(path.size() >= 1);

    // Ruta hacia tile lejano con tiles intermedios no-residentes
    // (2048..3072 = tile 2 y 3 faltan) → pide carga, no devuelve camino.
    path.clear();
    streaming = false;
    bool ok = pf.find_path(0, 0, 0, 4096, 0, 0, path, streaming);
    CHECK(!ok);
    CHECK(streaming);
    CHECK(path.empty());
    // Al menos un tile intermedio quedó en cola de carga.
    CHECK(nm.pending().size() > 0);
}

static void test_tiles_in_radius() {
    std::cout << "4. Enumeración de tiles por radio (prefetch)...\n";

    NavMeshStreamer nm;
    std::vector<NavTileCoord> tiles;
    nm.tiles_in_radius(0, 0, 0, 1, tiles);
    CHECK(tiles.size() == 27); // cubo 3x3x3
}

int main() {
    std::cout << "--- Starting FluxDB Streaming NavMesh Test (#17) ---\n";
    test_coord_convention();
    test_residency_and_load_requests();
    test_pathfinding_streams_missing_tiles();
    test_tiles_in_radius();
    std::cout << "--- STREAMING NAVMESH TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}