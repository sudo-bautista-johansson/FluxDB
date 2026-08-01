// FluxDB — Feature #26: Sandboxed Mod Query/Script API
// Vista restringida del World por capabilities; queries batch validadas.
#include "../core/headers/mod_sandbox.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb::sandbox;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

int main() {
    std::cout << "--- Starting FluxDB Sandboxed Mod Query/Script API Test (#26) ---\n";

    MockWorld world;
    // 3 jugadores con Health (hp, regen) y 2 con Stamina (stamina).
    world.add_entity("Health", {100, 1});
    world.add_entity("Health", {80, 2});
    world.add_entity("Health", {60, 3});
    world.add_entity("Stamina", {50});
    world.add_entity("Stamina", {50});
    CHECK(world.entity_count("Health") == 3);
    CHECK(world.entity_count("Stamina") == 2);

    // Mod confiable: solo READ de Health, READ_WRITE de Stamina.
    CapabilitySet caps;
    caps.grant("Health", Permission::READ);
    caps.grant("Stamina", Permission::READ_WRITE);

    SandboxedWorldView view(world, caps);

    // Query válida: read Health.
    SandboxedQuery q1;
    q1.component = "Health";
    q1.fields = {"hp", "regen"};
    CHECK(view.build_query(q1));
    CHECK(q1.constructed);

    QueryResult r1 = view.run(q1);
    CHECK(r1.entities.size() == 3);
    CHECK(r1.columns.size() == 2);
    CHECK(std::fabs(r1.columns[0][0] - 100.0f) < 1e-5f);
    CHECK(std::fabs(r1.columns[0][2] - 60.0f) < 1e-5f);
    CHECK(std::fabs(r1.columns[1][1] - 2.0f) < 1e-5f);

    // Query INVALIDA: escribir Health sin permiso → rechazada en build.
    SandboxedQuery q_bad;
    q_bad.component = "Health";
    q_bad.allow_write = true;
    CHECK(!view.build_query(q_bad));
    CHECK(!q_bad.constructed);
    CHECK(q_bad.reject_reason.find("no write permission") != std::string::npos);

    // Incluso una query read de un componente no otorgado → rechazada.
    SandboxedQuery q_nope;
    q_nope.component = "Inventory";
    q_nope.fields = {"slot"};
    CHECK(!view.build_query(q_nope));
    CHECK(q_nope.reject_reason.find("no read permission") != std::string::npos);

    // El sandbox NO "ve" componentes fuera de capabilities.
    CHECK(view.can_see("Health"));
    CHECK(view.can_see("Stamina"));
    CHECK(!view.can_see("Inventory"));

    // Batch write a Stamina (permitido): regen a 25 para todas.
    SandboxedQuery q2;
    q2.component = "Stamina";
    q2.allow_write = true;
    q2.write_fields = {"stamina"};
    CHECK(view.build_query(q2));
    size_t n = view.update_matching(q2, [](const std::vector<float>&) { return 25.0f; });
    CHECK(n == 2);

    SandboxedQuery q_read2;
    q_read2.component = "Stamina";
    q_read2.fields = {"stamina"};
    CHECK(view.build_query(q_read2));
    QueryResult r2 = view.run(q_read2);
    CHECK(std::fabs(r2.columns[0][0] - 25.0f) < 1e-5f);
    CHECK(std::fabs(r2.columns[0][1] - 25.0f) < 1e-5f);

    // Ejecutar una query no construida (rechazada) devuelve vacío, no crash.
    QueryResult empty_res = view.run(q_bad);
    CHECK(empty_res.entities.empty());

    // Un mod SIN capabilities no puede construir NADA.
    CapabilitySet empty_caps;
    SandboxedWorldView strict(world, empty_caps);
    SandboxedQuery q3;
    q3.component = "Health";
    q3.fields = {"hp"};
    CHECK(!strict.build_query(q3));
    CHECK(strict.run(q3).entities.empty());

    std::cout << "--- SANDBOXED MOD QUERY/Script API TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}