#pragma once

// ─────────────────────────────────────────────────────────────
//  Unified Delta Engine (#7) — one diff format, three sinks
// ─────────────────────────────────────────────────────────────
// A single binary diff format operates over the archetype
// storage's versioning system (#4): a DeltaSet records changed
// component bytes since a reference tick, with per-component
// compression strategies (RAW, QUANTIZED_FLOAT, RLE).
//
// Networking, replay recording, and save serialization are just
// three different sinks writing/reading the same DeltaSet format:
//   - Network : DeltaCompression (core/network/delta_compression.cpp)
//   - Replay  : ReplayRecorder / ReplayPlayer (this file)
//   - Save    : World::save_incremental / World::load_from_replay
//
// Serialized DeltaSet format (little endian):
//   [uint32 magic 0x44535444 'DSTD']
//   [uint8  version = 2]
//   [uint64 base_tick]      // tick at which this set starts
//   [uint64 end_tick]       // tick at which this set was produced
//   [uint32 num_codecs]     // codec table (only non-RAW present)
//     per entry: [uint8 comp_id][uint8 codec_id]
//   [uint32 num_records]
//   per record:
//     [uint8  op]           // 0=UPDATE, 1=SPAWN, 2=DESPAWN, 3=RELATION
//     [uint32 entity]       // src for RELATION
//     [uint8  comp_id]      // 0xFF for SPAWN/DESPAWN of whole entity;
//                           // RelationKind for RELATION
//     [uint32 data_size]    // encoded bytes
//     [uint32 orig_size]    // original component bytes
//     [byte[] data]         // RELATION: [u8 action][u32 dst][u8 payload_len][payload]
//
// Replay/save file format:
//   [uint32 magic 0x464C5852 'FLXR']
//   [uint32 version = 2]
//   [uint64 start_tick]             // tick of the base snapshot
//   [uint32 num_comps]              // component registration table
//     per comp: [uint8 id][uint8 codec_id][uint8 name_len][name][uint32 size]
//   [uint32 snapshot_len][DeltaSet snapshot]  // full state as SPAWN records
//   [uint32 num_ticks]
//   per tick: [uint32 len][DeltaSet]

#include <cstdint>
#include <cstddef>
#include <vector>
#include <array>
#include <string>
#include <fstream>
#include <utility>
#include "ecs.h"
#include "delta_codec.h"

namespace fluxdb {
namespace delta {

// ── Operations ────────────────────────────────────────────────

enum class DeltaOp : uint8_t {
    UPDATE = 0,   // component value changed on an existing entity
    SPAWN = 1,    // entity appeared (optionally with component data)
    DESPAWN = 2,  // entity disappeared
    RELATION = 3, // native relation edge changed (#6): (src, kind, dst) + payload
};

// ── Per-component compression strategies (#7) ─────────────────
// CodecID y el trait DeltaCodec<T> viven en delta_codec.h.

// Single record inside a DeltaSet.
struct DeltaRecord {
    DeltaOp op = DeltaOp::UPDATE;
    uint32_t entity = 0;
    ecs::ComponentID comp_id = 255;
    std::vector<uint8_t> data;   // decoded (original) bytes after deserialize
    uint32_t orig_size = 0;      // original component byte size
};

// Per-component codec configuration. Defaults to RAW for everything;
// components opt in via World::set_codec or DeltaCodec-style traits.
class CodecRegistry {
public:
    void set_codec(ecs::ComponentID comp_id, CodecID codec) {
        if (comp_id < ecs::MAX_COMPONENTS) {
            codecs_[comp_id] = codec;
        }
    }
    CodecID get_codec(ecs::ComponentID comp_id) const {
        if (comp_id >= ecs::MAX_COMPONENTS) return CodecID::RAW;
        return codecs_[comp_id];
    }

    // Encodes `src` (original bytes) appending encoded bytes to `out`.
    bool encode(ecs::ComponentID comp_id, const void* src, size_t size, std::vector<uint8_t>& out) const;

    // Decodes `src` (encoded bytes) into `dst` of `orig_size` bytes.
    bool decode(ecs::ComponentID comp_id, void* dst, size_t orig_size, const uint8_t* src, size_t src_size) const;

private:
    std::array<CodecID, ecs::MAX_COMPONENTS> codecs_{};
};

// ── DeltaSet: the unified change container ────────────────────

class DeltaSet {
public:
    static constexpr uint32_t kMagic = 0x44535444; // 'DSTD'
    static constexpr uint8_t kVersion = 2;

    explicit DeltaSet(uint64_t base_tick = 0);

    void add_update(uint32_t entity, ecs::ComponentID comp_id, const void* data, size_t size);
    void add_spawn(uint32_t entity, ecs::ComponentID comp_id = 255, const void* data = nullptr, size_t size = 0);
    void add_despawn(uint32_t entity);

    // Native relation events (#6): el kind viaja en comp_id.
    void add_relation(uint32_t src, ecs::RelationKind kind, uint32_t dst, ecs::RelationPayload payload = {});
    void add_relation_remove(uint32_t src, ecs::RelationKind kind, uint32_t dst);

    bool empty() const { return records_.empty(); }
    size_t record_count() const { return records_.size(); }
    uint64_t base_tick() const { return base_tick_; }
    uint64_t end_tick() const { return end_tick_; }
    void set_end_tick(uint64_t t) { end_tick_ = t; }

    void clear();

    // Encodes the set (applying the registry's per-component codecs) into `out`.
    void serialize(const CodecRegistry& codecs, std::vector<uint8_t>& out) const;

    // Parses `data`; decoded records are restored to original bytes.
    bool deserialize(const CodecRegistry& codecs, const uint8_t* data, size_t size);

    // Applies the set to a world (deterministic: spawn/update/despawn).
    // Records must already be decoded (call deserialize first).
    void apply(ecs::World& world) const;

    template <typename F>
    void for_each_record(F&& f) const {
        for (const auto& r : records_) f(r);
    }

private:
    uint64_t base_tick_ = 0;
    uint64_t end_tick_ = 0;
    std::vector<DeltaRecord> records_;
};

// ── Capture helpers (shared by replay, save, and rollback) ────

// Captura el estado completo del world como DeltaSet de SPAWN records
// (una entidad desnuda + un record por componente). Es la base tanto de
// snapshots de replay/save como del SnapshotRingBuffer de rollback (#8).
DeltaSet capture_world_snapshot(const ecs::World& world);

// Captura los cambios estrictamente posteriores a `since_tick`: eventos
// estructurales (spawn/despawn) + writes de componentes vía el versionado
// de #4 + eventos de relación (#6, con tombstones para removals).
// Es el mismo diff que ReplayRecorder::record_tick y el ring de #8.
DeltaSet capture_tick_delta(const ecs::World& world, uint64_t since_tick);

// ── Save compaction (#7) ───────────────────────────────────────

// Pliega la cadena de deltas de un replay/save en su snapshot base:
// lee el archivo `in_path`, aplica todos los ticks a la snapshot y escribe
// `out_path` con un ÚNICO snapshot del estado final y 0 ticks (last-write-wins
// por (entity, comp), los DESPAWN eliminan el estado previo del entity).
// Resultado: archivo autocontenido y compacto, cargable con ReplayPlayer.
bool fold_replay_file(const std::string& in_path, const std::string& out_path);

// ── Sink 1: Replay recording (also powers incremental saves) ──

class ReplayRecorder {
public:
    static constexpr uint32_t kReplayMagic = 0x464C5852; // 'FLXR'

    ReplayRecorder() = default;
    explicit ReplayRecorder(const std::string& path);
    ~ReplayRecorder();

    bool is_open() const { return file_.is_open(); }
    bool open(const std::string& path);

    // Writes the file header, component table, and a full-state snapshot.
    void begin_recording(ecs::World& world);

    // Writes the DeltaSet of all changes stamped strictly after the
    // previous record point. Canonical flow:
    //   world.advance_tick(); mutate(); recorder.record_tick(world);
    void record_tick(ecs::World& world);

    void close();

private:
    std::ofstream file_;
    uint64_t last_tick_ = 0;
    uint64_t ticks_written_ = 0;
    std::streampos ticks_pos_{};
    bool recording_ = false;
};

// ── Sink 2: Replay / save loading ─────────────────────────────

class ReplayPlayer {
public:
    static constexpr uint32_t kReplayMagic = 0x464C5852; // 'FLXR'

    ReplayPlayer() = default;
    ~ReplayPlayer();

    bool open(const std::string& path);

    // Registers the file's components (and codecs) into the world's store.
    void load_components(ecs::World& world) const;

    // Applies the snapshot, then one tick per call. Returns false at end.
    bool step(ecs::World& world);

    bool finished() const {
        return opened_ && snapshot_applied_ && tick_index_ >= tick_bufs_.size();
    }
    uint64_t start_tick() const { return start_tick_; }

    // Save compaction (#7): pliega la cadena de deltas leyendo los buffers
    // internos del player (acceso de amigo, misma TU).
    friend bool fold_replay_file(const std::string& in_path, const std::string& out_path);

private:
    struct ComponentDef {
        uint8_t id;
        uint8_t codec;
        std::string name;
        uint32_t size;
    };

    std::ifstream file_;
    bool opened_ = false;
    uint64_t start_tick_ = 0;
    std::vector<ComponentDef> comps_;
    std::vector<uint8_t> snap_buf_;
    std::vector<std::vector<uint8_t>> tick_bufs_;
    CodecRegistry codecs_;
    bool snapshot_applied_ = false;
    size_t tick_index_ = 0;
};

} // namespace delta
} // namespace fluxdb
