// FluxDB — Feature #25: Data-Oriented Mod Overlays
// Resolución de namespace de IDs + fusión de overlays + hash de schema.
#include "../core/headers/mod_overlay.h"
#include <iostream>
#include <cassert>
#include <string>

using namespace fluxdb::mod;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

int main() {
    std::cout << "--- Starting FluxDB Data-Oriented Mod Overlays Test (#25) ---\n";

    ModRegistry reg;

    // Componentes core.
    uint16_t pos_id = reg.register_core_component("Transform");
    uint16_t hp_id = reg.register_core_component("Health");
    CHECK(pos_id == 0);
    CHECK(hp_id == 1);

    // Mod 1: parchea Health (regen) y añade "Buffable".
    ResolvedMod m1;
    m1.mod_id = "combat_pack";
    OverlayComponent patch_health;
    patch_health.name = "Health_Overlay";
    patch_health.is_patch = true;
    patch_health.base_name = "Health";
    patch_health.fields.push_back({"regen", FieldKind::FLOAT});
    m1.components.push_back(patch_health);

    OverlayComponent buffable;
    buffable.name = "Buffable";
    buffable.fields.push_back({"stack", FieldKind::UINT});
    m1.components.push_back(buffable);

    CHECK(reg.resolve(m1));
    CHECK(m1.components[0].resolved_id == hp_id);       // patch → id core
    CHECK(m1.components[1].resolved_id == kModIdBase);  // nuevo → namespace mod
    CHECK(m1.schema_hash != 0);

    // Mod 2: parchea Transform y añade otro componente → ID distinto.
    ResolvedMod m2;
    m2.mod_id = "mobility_pack";
    OverlayComponent patch_transform;
    patch_transform.name = "Transform_Overlay";
    patch_transform.is_patch = true;
    patch_transform.base_name = "Transform";
    patch_transform.fields.push_back({"max_speed", FieldKind::FLOAT});
    m2.components.push_back(patch_transform);

    OverlayComponent jetpack;
    jetpack.name = "Jetpack";
    jetpack.fields.push_back({"fuel", FieldKind::FLOAT});
    m2.components.push_back(jetpack);

    CHECK(reg.resolve(m2));
    CHECK(m2.components[0].resolved_id == pos_id);
    CHECK(m2.components[1].resolved_id == kModIdBase + 1); // sin colisión

    // No hay colisión de ownership.
    CHECK(reg.owner_of_component(m1.components[1].resolved_id) == "combat_pack");
    CHECK(reg.owner_of_component(m2.components[1].resolved_id) == "mobility_pack");

    // Patch a componente inexistente → resolución rechazada.
    ResolvedMod bad;
    bad.mod_id = "broken_mod";
    OverlayComponent bad_patch;
    bad_patch.name = "X";
    bad_patch.is_patch = true;
    bad_patch.base_name = "Nonexistent";
    bad.components.push_back(bad_patch);
    CHECK(!reg.resolve(bad));

    // Duplicado de mod_id → rechazado (idempotencia).
    ResolvedMod dup = m1;
    dup.mod_id = "combat_pack"; // ya resuelto
    CHECK(!reg.resolve(dup));

    // Duplicado interno de nombre de componente → rechazado.
    ResolvedMod dup_comp;
    dup_comp.mod_id = "dup_comp";
    OverlayComponent c1, c2;
    c1.name = "SameName";
    c2.name = "SameName";
    c2.fields.push_back({"a", FieldKind::FLOAT});
    dup_comp.components.push_back(c1);
    dup_comp.components.push_back(c2);
    CHECK(!reg.resolve(dup_comp));

    // Byte sizes de campos.
    CHECK(patch_health.fields[0].byte_size() == 4);
    CHECK(buffable.fields[0].byte_size() == 4);
    OverlayField f3 = {"normal", FieldKind::FLOAT3};
    CHECK(f3.byte_size() == 12);

    // Hash de schema: mods idénticos en orden distinto → MISMOS componentes
    // por ID, hash global estable.
    ModRegistry reg2;
    reg2.register_core_component("Transform");
    reg2.register_core_component("Health");
    ResolvedMod r2_m1 = m1;
    r2_m1.mod_id = "combat_pack";
    r2_m1.schema_hash = 0;
    reg2.resolve(r2_m1);
    ResolvedMod r2_m2 = m2;
    r2_m2.mod_id = "mobility_pack";
    r2_m2.schema_hash = 0;
    reg2.resolve(r2_m2);

    // Mismos mods en otro registry → mismo schema hash global.
    CHECK(reg.global_schema_hash() == reg2.global_schema_hash());

    std::cout << "--- MOD OVERLAYS TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}