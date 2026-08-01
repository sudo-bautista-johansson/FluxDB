#pragma once

#include <vector>
#include <cstdint>
#include <memory>
#include "ecs.h"

namespace fluxdb {
namespace network {

// Helper class to extract binary deltas from the ECS for synchronization 
// over UDP sockets (Unreal Engine / Unity clients).
//
// This is the NETWORK sink of the Unified Delta Engine (#7): the payload
// is a serialized DeltaSet (core/headers/delta_set.h), the same format
// used by replay recording and incremental saves. A delta received over
// the wire can be deserialized and applied directly to a World:
//
//   DeltaSet set;
//   set.deserialize(world.codec_registry(), data, len);
//   set.apply(world);
class DeltaCompression {
public:
    DeltaCompression(ecs::World* world) : world_(world) {}

    // Generates a DeltaSet containing:
    //   - SPAWN/DESPAWN structural events strictly after `last_ack_tick`
    //   - component data written strictly after `last_ack_tick` (via #4)
    // Serialized format (see delta_set.h):
    //   [uint32 magic][uint8 version][uint64 base_tick][uint64 end_tick]
    //   [uint32 num_codecs][table][uint32 num_records]
    //   per record: [uint8 op][uint32 entity][uint8 comp_id][uint32 data_size]
    //               [uint32 orig_size][byte[] payload...]
    // Consumers should ack payload.end_tick() to advance the ack watermark.
    void generate_delta_payload(uint64_t last_ack_tick, std::vector<uint8_t>& out_buffer) const;

private:
    ecs::World* world_;
};

} // namespace network
} // namespace fluxdb
