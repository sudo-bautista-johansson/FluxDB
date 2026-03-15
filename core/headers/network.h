#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include "ecs.h"

namespace fluxdb {
namespace network {

// Helper class to extract binary deltas from the ECS for synchronization 
// over UDP sockets (Unreal Engine / Unity clients).
class DeltaCompression {
public:
    DeltaCompression(ecs::World* world) : world_(world) {}

    // Generates a compact binary payload containing the *current live state*
    // of all entity components that have changed strictly after `last_ack_tick`.
    // Format:
    // [Header: uint32 num_records]
    // For each record:
    //   [uint32 entity_id]
    //   [uint8 component_id]
    //   [uint8 data_size]
    //   [byte[] payload...]
    void generate_delta_payload(uint64_t last_ack_tick, std::vector<uint8_t>& out_buffer) const;

private:
    ecs::World* world_;
};

} // namespace network
} // namespace fluxdb
