#include "../headers/network.h"
#include "../headers/delta_set.h"

namespace fluxdb {
namespace network {

void DeltaCompression::generate_delta_payload(uint64_t last_ack_tick, std::vector<uint8_t>& out_buffer) const {
    out_buffer.clear();

    if (!world_) {
        return;
    }

    // Unified Delta Engine (#7): the network sink emits the same DeltaSet
    // format that replay recording and incremental saves use.
    // El backbone de detección de cambios es el versionado (#4):
    // filtro grueso por arquetipo en O(1) + granularidad fina por entidad.
    delta::DeltaSet set(last_ack_tick);
    set.set_end_tick(world_->current_tick());

    // Eventos estructurales (spawn/despawn) sellados después del ack.
    for (const auto& ev : world_->structural_events()) {
        if (ev.tick > last_ack_tick) {
            if (ev.spawned) {
                set.add_spawn(ev.entity);
            } else {
                set.add_despawn(ev.entity);
            }
        }
    }

    // Componentes modificados después del ack.
    fluxdb::ecs::ComponentStore* store = world_->get_store();
    size_t num_comps = store->count();
    for (fluxdb::ecs::ComponentID comp = 0; comp < num_comps; ++comp) {
        world_->for_each_changed(comp, last_ack_tick,
            [&](uint32_t entity, size_t row, uint32_t last_write_tick) {
                size_t comp_size = 0;
                const void* data = world_->get_entity_component_data(entity, comp, comp_size);
                if (data) {
                    set.add_update(entity, comp, data, comp_size);
                }
            });
    }

    set.serialize(world_->codec_registry(), out_buffer);
}

} // namespace network
} // namespace fluxdb
