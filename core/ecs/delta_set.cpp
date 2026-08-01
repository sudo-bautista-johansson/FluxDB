#include "../headers/delta_set.h"
#include "../headers/sector_pos.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fluxdb {
namespace delta {

// ── Codec implementations ─────────────────────────────────────

namespace {

bool encode_quantized_float(const void* src, size_t size, std::vector<uint8_t>& out) {
    if (size % 4 != 0 || size == 0) return false;
    size_t n = size / 4;
    const float* f = static_cast<const float*>(src);

    float max_abs = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        max_abs = std::max(max_abs, std::fabs(f[i]));
    }
    float scale = (max_abs > 0.0f) ? (max_abs / 32767.0f) : 1.0f;

    size_t start = out.size();
    out.resize(start + 4 + 2 * n);
    std::memcpy(out.data() + start, &scale, 4);
    int16_t* q = reinterpret_cast<int16_t*>(out.data() + start + 4);
    for (size_t i = 0; i < n; ++i) {
        float v = f[i] / scale;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32767.0f) v = -32767.0f;
        q[i] = static_cast<int16_t>(std::round(v));
    }
    return true;
}

bool decode_quantized_float(void* dst, size_t orig_size, const uint8_t* src, size_t src_size) {
    if (orig_size % 4 != 0 || orig_size == 0) return false;
    size_t n = orig_size / 4;
    if (src_size != 4 + 2 * n) return false;

    float scale = 1.0f;
    std::memcpy(&scale, src, 4);
    const int16_t* q = reinterpret_cast<const int16_t*>(src + 4);
    float* f = static_cast<float*>(dst);
    for (size_t i = 0; i < n; ++i) {
        f[i] = static_cast<float>(q[i]) * scale;
    }
    return true;
}

bool encode_rle(const void* src, size_t size, std::vector<uint8_t>& out) {
    const uint8_t* p = static_cast<const uint8_t*>(src);
    std::vector<std::pair<uint8_t, uint32_t>> runs;
    for (size_t i = 0; i < size; ) {
        uint8_t v = p[i];
        size_t j = i;
        while (j < size && p[j] == v) ++j;
        runs.emplace_back(v, static_cast<uint32_t>(j - i));
        i = j;
    }

    size_t start = out.size();
    uint32_t num_runs = static_cast<uint32_t>(runs.size());
    out.resize(start + 4 + runs.size() * 5);
    std::memcpy(out.data() + start, &num_runs, 4);
    uint8_t* w = out.data() + start + 4;
    for (const auto& run : runs) {
        *w++ = run.first;
        std::memcpy(w, &run.second, 4);
        w += 4;
    }
    return true;
}

bool decode_rle(void* dst, size_t orig_size, const uint8_t* src, size_t src_size) {
    if (src_size < 4) return false;
    uint32_t num_runs = 0;
    std::memcpy(&num_runs, src, 4);
    if (src_size != 4 + static_cast<size_t>(num_runs) * 5) return false;

    uint8_t* w = static_cast<uint8_t*>(dst);
    size_t written = 0;
    const uint8_t* p = src + 4;
    for (uint32_t i = 0; i < num_runs; ++i) {
        uint8_t v = *p++;
        uint32_t count = 0;
        std::memcpy(&count, p, 4);
        p += 4;
        if (written + count > orig_size) return false;
        std::memset(w + written, v, count);
        written += count;
    }
    return written == orig_size;
}

// ── Bit-packing (#7) ──────────────────────────────────────────
// Arrays de enteros (u8/u16/u32/u64): calcula el ancho de bits máximo y
// empaqueta los valores en ese ancho. Formato:
//   [u8 value_size][u8 bit_width][u32 num_values][bits empaquetados]

void put_bits(uint8_t* buf, size_t& bit_pos, uint64_t v, uint8_t width) {
    for (uint8_t b = 0; b < width; ++b) {
        if (v & (1ULL << b)) {
            buf[(bit_pos + b) / 8] |= static_cast<uint8_t>(1u << ((bit_pos + b) % 8));
        }
    }
    bit_pos += width;
}

uint64_t get_bits(const uint8_t* buf, size_t& bit_pos, uint8_t width) {
    uint64_t v = 0;
    for (uint8_t b = 0; b < width; ++b) {
        if (buf[(bit_pos + b) / 8] & (1u << ((bit_pos + b) % 8))) {
            v |= (1ULL << b);
        }
    }
    bit_pos += width;
    return v;
}

uint64_t read_value(const uint8_t* p, size_t value_size) {
    uint64_t v = 0;
    std::memcpy(&v, p, value_size);
    return v;
}

void write_value(uint8_t* p, size_t value_size, uint64_t v) {
    std::memcpy(p, &v, value_size);
}

bool encode_bitpack(const void* src, size_t size, std::vector<uint8_t>& out) {
    if (size == 0) return false;
    size_t value_size;
    if (size % 8 == 0) value_size = 8;
    else if (size % 4 == 0) value_size = 4;
    else if (size % 2 == 0) value_size = 2;
    else value_size = 1;
    size_t n = size / value_size;
    const uint8_t* p = static_cast<const uint8_t*>(src);

    uint64_t max_value = 0;
    for (size_t i = 0; i < n; ++i) {
        max_value = std::max(max_value, read_value(p + i * value_size, value_size));
    }
    uint8_t bit_width = 0;
    for (uint64_t t = max_value; t > 0; t >>= 1) ++bit_width;

    size_t start = out.size();
    size_t packed_bytes = (n * bit_width + 7) / 8;
    out.resize(start + 1 + 1 + 4 + packed_bytes);
    uint8_t* w = out.data() + start;
    w[0] = static_cast<uint8_t>(value_size);
    w[1] = bit_width;
    std::memcpy(w + 2, &n, 4);

    size_t bit_pos = 0;
    for (size_t i = 0; i < n; ++i) {
        put_bits(w + 6, bit_pos, read_value(p + i * value_size, value_size), bit_width);
    }
    return true;
}

bool decode_bitpack(void* dst, size_t orig_size, const uint8_t* src, size_t src_size) {
    if (src_size < 6) return false;
    size_t value_size = src[0];
    uint8_t bit_width = src[1];
    uint32_t n = 0;
    std::memcpy(&n, src + 2, 4);
    if (value_size == 0 || value_size > 8) return false;
    if (static_cast<size_t>(n) * value_size != orig_size) return false;
    if (src_size != 6 + (n * bit_width + 7) / 8) return false;

    uint8_t* w = static_cast<uint8_t*>(dst);
    size_t bit_pos = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint64_t v = bit_width ? get_bits(src + 6, bit_pos, bit_width) : 0;
        write_value(w + i * value_size, value_size, v);
    }
    return true;
}

} // namespace

// ── SectorPos codec (#19) ─────────────────────────────────────
// Sector-relative positions comprimen mejor que floats de mundo gigantes:
//   [int32 sx][int32 sy][int32 sz]   (12 bytes: sectores enteros)
//   [float scale][int16 ox][int16 oy][int16 oz]  (10 bytes: offsets locales)
// Total 22 bytes contra 24 del RAW — y la PRECISIÓN es mejor: los offsets
// viven en [-512,512) → escala ~0.0156 unidades; cuantizar floats de mundo
// a 1e9 de distancia tendría precisión de cientos de unidades.

bool encode_sector_pos(const void* src, size_t size, std::vector<uint8_t>& out) {
    if (size != sizeof(ecs::SectorPos)) return false;
    const ecs::SectorPos* p = static_cast<const ecs::SectorPos*>(src);

    size_t start = out.size();
    out.resize(start + 22);
    uint8_t* w = out.data() + start;
    std::memcpy(w, &p->sx, 4); std::memcpy(w + 4, &p->sy, 4); std::memcpy(w + 8, &p->sz, 4);

    float scale = ecs::SECTOR_HALF / 32767.0f;
    std::memcpy(w + 12, &scale, 4);
    auto clamp16 = [](float v) {
        if (v > 32767.0f) return 32767.0f;
        if (v < -32767.0f) return -32767.0f;
        return v;
    };
    float qx = clamp16(p->ox / scale), qy = clamp16(p->oy / scale), qz = clamp16(p->oz / scale);
    int16_t iqx = static_cast<int16_t>(std::lround(qx));
    int16_t iqy = static_cast<int16_t>(std::lround(qy));
    int16_t iqz = static_cast<int16_t>(std::lround(qz));
    std::memcpy(w + 16, &iqx, 2); std::memcpy(w + 18, &iqy, 2); std::memcpy(w + 20, &iqz, 2);
    return true;
}

bool decode_sector_pos(void* dst, size_t orig_size, const uint8_t* src, size_t src_size) {
    if (orig_size != sizeof(ecs::SectorPos) || src_size != 22) return false;
    ecs::SectorPos* p = static_cast<ecs::SectorPos*>(dst);
    std::memcpy(&p->sx, src, 4); std::memcpy(&p->sy, src + 4, 4); std::memcpy(&p->sz, src + 8, 4);
    float scale = 1.0f;
    std::memcpy(&scale, src + 12, 4);
    int16_t qx, qy, qz;
    std::memcpy(&qx, src + 16, 2); std::memcpy(&qy, src + 18, 2); std::memcpy(&qz, src + 20, 2);
    p->ox = static_cast<float>(qx) * scale;
    p->oy = static_cast<float>(qy) * scale;
    p->oz = static_cast<float>(qz) * scale;
    return true;
}

// ── CodecRegistry ─────────────────────────────────────────────

bool CodecRegistry::encode(ecs::ComponentID comp_id, const void* src, size_t size, std::vector<uint8_t>& out) const {
    switch (get_codec(comp_id)) {
        case CodecID::RAW: {
            size_t start = out.size();
            out.resize(start + size);
            std::memcpy(out.data() + start, src, size);
            return true;
        }
        case CodecID::QUANTIZED_FLOAT:
            return encode_quantized_float(src, size, out);
        case CodecID::RLE:
            return encode_rle(src, size, out);
        case CodecID::BITPACK:
            return encode_bitpack(src, size, out);
        case CodecID::SECTOR_POS:
            return encode_sector_pos(src, size, out);
    }
    return false;
}

bool CodecRegistry::decode(ecs::ComponentID comp_id, void* dst, size_t orig_size, const uint8_t* src, size_t src_size) const {
    switch (get_codec(comp_id)) {
        case CodecID::RAW: {
            if (src_size != orig_size) return false;
            std::memcpy(dst, src, orig_size);
            return true;
        }
        case CodecID::QUANTIZED_FLOAT:
            return decode_quantized_float(dst, orig_size, src, src_size);
        case CodecID::RLE:
            return decode_rle(dst, orig_size, src, src_size);
        case CodecID::BITPACK:
            return decode_bitpack(dst, orig_size, src, src_size);
        case CodecID::SECTOR_POS:
            return decode_sector_pos(dst, orig_size, src, src_size);
    }
    return false;
}

// ── DeltaSet ──────────────────────────────────────────────────

DeltaSet::DeltaSet(uint64_t base_tick) : base_tick_(base_tick), end_tick_(base_tick) {}

void DeltaSet::add_update(uint32_t entity, ecs::ComponentID comp_id, const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    DeltaRecord r;
    r.op = DeltaOp::UPDATE;
    r.entity = entity;
    r.comp_id = comp_id;
    r.data.assign(p, p + size);
    r.orig_size = static_cast<uint32_t>(size);
    records_.push_back(std::move(r));
}

void DeltaSet::add_spawn(uint32_t entity, ecs::ComponentID comp_id, const void* data, size_t size) {
    DeltaRecord r;
    r.op = DeltaOp::SPAWN;
    r.entity = entity;
    r.comp_id = comp_id;
    if (data && size > 0) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        r.data.assign(p, p + size);
    }
    r.orig_size = static_cast<uint32_t>(size);
    records_.push_back(std::move(r));
}

void DeltaSet::add_despawn(uint32_t entity) {
    DeltaRecord r;
    r.op = DeltaOp::DESPAWN;
    r.entity = entity;
    r.comp_id = 255;
    r.orig_size = 0;
    records_.push_back(std::move(r));
}

namespace {
// data = [u8 action][u32 dst][u8 payload_len][payload]
constexpr uint8_t kRelationActionAdd = 0;
constexpr uint8_t kRelationActionRemove = 1;

void build_relation_data(std::vector<uint8_t>& data, uint8_t action, uint32_t dst,
                         const ecs::RelationPayload& payload) {
    data.resize(1 + 4 + 1 + 8);
    data[0] = action;
    std::memcpy(data.data() + 1, &dst, 4);
    data[5] = 0; // payload_len
    uint64_t raw = payload.raw;
    if (raw != 0) {
        data[5] = 8;
        std::memcpy(data.data() + 6, &raw, 8);
    }
    data.resize(6 + data[5]);
}
} // namespace

void DeltaSet::add_relation(uint32_t src, ecs::RelationKind kind, uint32_t dst, ecs::RelationPayload payload) {
    DeltaRecord r;
    r.op = DeltaOp::RELATION;
    r.entity = src;
    r.comp_id = static_cast<ecs::ComponentID>(kind & 0xFF);
    r.orig_size = 0;
    build_relation_data(r.data, kRelationActionAdd, dst, payload);
    records_.push_back(std::move(r));
}

void DeltaSet::add_relation_remove(uint32_t src, ecs::RelationKind kind, uint32_t dst) {
    DeltaRecord r;
    r.op = DeltaOp::RELATION;
    r.entity = src;
    r.comp_id = static_cast<ecs::ComponentID>(kind & 0xFF);
    r.orig_size = 0;
    build_relation_data(r.data, kRelationActionRemove, dst, ecs::RelationPayload{});
    records_.push_back(std::move(r));
}

void DeltaSet::clear() {
    records_.clear();
    base_tick_ = 0;
    end_tick_ = 0;
}

void DeltaSet::serialize(const CodecRegistry& codecs, std::vector<uint8_t>& out) const {
    out.clear();

    // Codec table: only components whose codec differs from RAW.
    std::array<uint8_t, ecs::MAX_COMPONENTS> table{};
    size_t table_count = 0;
    for (const auto& r : records_) {
        CodecID c = codecs.get_codec(r.comp_id);
        if (c == CodecID::RAW) continue;
        bool present = false;
        for (size_t i = 0; i < table_count; ++i) {
            if (table[i] == r.comp_id) { present = true; break; }
        }
        if (!present && table_count < ecs::MAX_COMPONENTS) {
            table[table_count++] = r.comp_id;
        }
    }

    size_t o = 0;
    size_t header = 4 + 1 + 8 + 8 + 4 + table_count * 2 + 4;
    out.resize(header);
    std::memcpy(out.data() + o, &kMagic, 4); o += 4;
    out[o++] = kVersion;
    std::memcpy(out.data() + o, &base_tick_, 8); o += 8;
    std::memcpy(out.data() + o, &end_tick_, 8); o += 8;
    uint32_t num_codecs = static_cast<uint32_t>(table_count);
    std::memcpy(out.data() + o, &num_codecs, 4); o += 4;
    for (size_t i = 0; i < table_count; ++i) {
        out[o++] = table[i];
        out[o++] = static_cast<uint8_t>(codecs.get_codec(table[i]));
    }
    uint32_t num_records = static_cast<uint32_t>(records_.size());
    std::memcpy(out.data() + o, &num_records, 4); o += 4;

    for (const auto& r : records_) {
        std::vector<uint8_t> encoded;
        if (!r.data.empty()) {
            if (r.op == DeltaOp::RELATION) {
                encoded = r.data; // datos de arista pre-armados (sin codecs de componente)
            } else {
                codecs.encode(r.comp_id, r.data.data(), r.data.size(), encoded);
            }
        }
        uint32_t ds = static_cast<uint32_t>(encoded.size());
        uint32_t os = r.orig_size;
        size_t rec_size = 1 + 4 + 1 + 4 + 4 + ds;
        out.resize(out.size() + rec_size);

        out[o++] = static_cast<uint8_t>(r.op);
        std::memcpy(out.data() + o, &r.entity, 4); o += 4;
        out[o++] = r.comp_id;
        std::memcpy(out.data() + o, &ds, 4); o += 4;
        std::memcpy(out.data() + o, &os, 4); o += 4;
        if (ds > 0) {
            std::memcpy(out.data() + o, encoded.data(), ds);
            o += ds;
        }
    }
}

bool DeltaSet::deserialize(const CodecRegistry& codecs, const uint8_t* data, size_t size) {
    clear();
    if (size < 25) return false;

    size_t o = 0;
    uint32_t magic = 0;
    std::memcpy(&magic, data + o, 4); o += 4;
    if (magic != kMagic) return false;
    uint8_t version = data[o++];
    if (version < 1 || version > kVersion) return false;
    std::memcpy(&base_tick_, data + o, 8); o += 8;
    std::memcpy(&end_tick_, data + o, 8); o += 8;
    uint32_t num_codecs = 0;
    std::memcpy(&num_codecs, data + o, 4); o += 4;
    if (o + static_cast<size_t>(num_codecs) * 2 + 4 > size) return false;
    o += static_cast<size_t>(num_codecs) * 2; // table is informational; registry is authoritative

    uint32_t num_records = 0;
    std::memcpy(&num_records, data + o, 4); o += 4;

    for (uint32_t i = 0; i < num_records; ++i) {
        if (o + 14 > size) return false;
        DeltaRecord r;
        uint8_t op = data[o++];
        if (op > static_cast<uint8_t>(DeltaOp::RELATION)) return false;
        r.op = static_cast<DeltaOp>(op);
        std::memcpy(&r.entity, data + o, 4); o += 4;
        r.comp_id = data[o++];
        uint32_t ds = 0, os = 0;
        std::memcpy(&ds, data + o, 4); o += 4;
        std::memcpy(&os, data + o, 4); o += 4;
        if (o + ds > size) return false;

        if (r.op != DeltaOp::DESPAWN && r.op != DeltaOp::RELATION && ds > 0) {
            // UPDATE y SPAWN con datos (p.ej. snapshots) llevan bytes encoded;
            // se decodifican de vuelta a los bytes originales del componente.
            r.orig_size = os;
            r.data.resize(os);
            if (!codecs.decode(r.comp_id, r.data.data(), os, data + o, ds)) {
                // Fallback: keep the encoded bytes verbatim.
                r.data.resize(ds);
                std::memcpy(r.data.data(), data + o, ds);
            }
        } else if (r.op == DeltaOp::RELATION && ds > 0) {
            // Evento de relación: datos verbatim (sin codecs de componente).
            r.orig_size = os;
            r.data.resize(ds);
            std::memcpy(r.data.data(), data + o, ds);
        }
        o += ds;
        records_.push_back(std::move(r));
    }
    return true;
}

void DeltaSet::apply(ecs::World& world) const {
    for (const auto& r : records_) {
        switch (r.op) {
            case DeltaOp::SPAWN:
                world.spawn_with_id(r.entity);
                if (!r.data.empty()) {
                    // SPAWN puede llevar datos (p.ej. snapshots de estado completo)
                    world.add_component(r.entity, r.comp_id, r.data.data());
                }
                break;
            case DeltaOp::DESPAWN:
                world.despawn(r.entity);
                break;
            case DeltaOp::UPDATE:
                if (!r.data.empty()) {
                    world.add_component(r.entity, r.comp_id, r.data.data());
                }
                break;
            case DeltaOp::RELATION: {
                // data = [u8 action][u32 dst][u8 payload_len][payload]
                if (r.data.size() < 6) break;
                uint8_t action = r.data[0];
                uint32_t dst = 0;
                std::memcpy(&dst, r.data.data() + 1, 4);
                uint8_t payload_len = r.data[5];
                ecs::RelationPayload payload;
                if (payload_len > 0 && r.data.size() >= 6 + payload_len) {
                    payload.raw = 0;
                    std::memcpy(&payload.raw, r.data.data() + 6, payload_len);
                }
                ecs::RelationKind kind = static_cast<ecs::RelationKind>(r.comp_id & 0xFF);
                if (action == kRelationActionAdd) {
                    world.add_relation(r.entity, kind, dst, payload);
                } else {
                    world.remove_relation(r.entity, kind, dst);
                }
                break;
            }
        }
    }
}

// ── Capture helpers ───────────────────────────────────────────

DeltaSet capture_world_snapshot(const ecs::World& world) {
    uint64_t tick = world.current_tick();
    DeltaSet snap(tick);
    snap.set_end_tick(tick);

    // Iteración canónica (#11): snapshots en orden de firma ascendente.
    // for_each_archetype_sorted toma el shared lock del World.
    const ecs::ComponentStore* store = world.get_store();
    world.for_each_archetype_sorted([&](ecs::Archetype* arch) {
        const ecs::Entity* entities = arch->get_entities_ptr();
        if (!entities) return;
        ecs::ArchetypeSignature sig = arch->get_signature();
        for (size_t row = 0; row < arch->get_entity_count(); ++row) {
            ecs::Entity ent = entities[row];
            // Spawn de la entidad en sí (cubre arquetipos sin componentes,
            // p.ej. entidades recién creadas que aún no tienen data).
            snap.add_spawn(ent);
            for (size_t i = 0; i < ecs::MAX_COMPONENTS; ++i) {
                if (sig.test(i)) {
                    ecs::ComponentID c = static_cast<ecs::ComponentID>(i);
                    void* data = arch->get_component_data(row, c);
                    snap.add_spawn(ent, c, data, store->get_info(c).size);
                }
            }
        }
    });

    // Aristas vivas (#6): después de los spawns (apply requiere entidades).
    world.relations().for_each_edge(
        [&](uint32_t src, ecs::RelationKind kind, uint32_t dst, const ecs::RelationPayload& payload) {
            snap.add_relation(src, kind, dst, payload);
        });
    return snap;
}

DeltaSet capture_tick_delta(const ecs::World& world, uint64_t since_tick) {
    DeltaSet set(since_tick);
    set.set_end_tick(world.current_tick());

    // Eventos estructurales (spawn/despawn) sellados después de `since_tick`.
    for (const auto& ev : world.structural_events()) {
        if (ev.tick > since_tick) {
            if (ev.spawned) {
                set.add_spawn(ev.entity);
            } else {
                set.add_despawn(ev.entity);
            }
        }
    }

    // Componentes modificados después de `since_tick` (versionado #4).
    const ecs::ComponentStore* store = world.get_store();
    for (ecs::ComponentID comp = 0; comp < store->count(); ++comp) {
        world.for_each_changed(comp, since_tick,
            [&](uint32_t entity, size_t, uint32_t) {
                size_t comp_size = 0;
                const void* data = world.get_entity_component_data(entity, comp, comp_size);
                if (data) {
                    set.add_update(entity, comp, data, comp_size);
                }
            });
    }

    // Relaciones (#6): aristas añadidas/actualizadas + removidas (tombstones),
    // ambas selladas por el versionado de #4.
    const ecs::RelationGraph& graph = world.relations();
    for (size_t k = 0; k < ecs::MAX_RELATION_KINDS; ++k) {
        ecs::RelationKind kind = static_cast<ecs::RelationKind>(k);
        if (!graph.kind_changed_since(kind, since_tick)) continue;

        graph.for_each_changed_src(kind, since_tick,
            [&](uint32_t src) {
                graph.for_each_outgoing(src, kind,
                    [&](uint32_t dst, const ecs::RelationPayload& payload) {
                        set.add_relation(src, kind, dst, payload);
                    });
            });
        graph.for_each_removed_since(kind, since_tick,
            [&](uint32_t src, uint32_t dst, const ecs::RelationPayload&) {
                set.add_relation_remove(src, kind, dst);
            });
    }
    return set;
}

// ── ReplayRecorder ────────────────────────────────────────────

ReplayRecorder::ReplayRecorder(const std::string& path) {
    open(path);
}

ReplayRecorder::~ReplayRecorder() {
    close();
}

bool ReplayRecorder::open(const std::string& path) {
    file_.open(path, std::ios::binary | std::ios::trunc);
    return file_.is_open();
}

void ReplayRecorder::begin_recording(ecs::World& world) {
    if (!file_.is_open()) return;
    recording_ = true;
    ticks_written_ = 0;
    last_tick_ = world.current_tick();

    uint32_t magic = kReplayMagic;
    uint32_t version = 2; // v2: DeltaSet puede incluir eventos RELATION (#6)
    file_.write(reinterpret_cast<const char*>(&magic), 4);
    file_.write(reinterpret_cast<const char*>(&version), 4);
    file_.write(reinterpret_cast<const char*>(&last_tick_), 8);

    // Component registration table (self-contained file format).
    ecs::ComponentStore* store = world.get_store();
    uint32_t num_comps = static_cast<uint32_t>(store->count());
    file_.write(reinterpret_cast<const char*>(&num_comps), 4);
    for (ecs::ComponentID c = 0; c < store->count(); ++c) {
        const ecs::ComponentInfo& info = store->get_info(c);
        uint8_t id = info.id;
        uint8_t codec = static_cast<uint8_t>(world.codec_registry().get_codec(c));
        uint8_t name_len = static_cast<uint8_t>(info.name.size());
        uint32_t size = static_cast<uint32_t>(info.size);
        file_.write(reinterpret_cast<const char*>(&id), 1);
        file_.write(reinterpret_cast<const char*>(&codec), 1);
        file_.write(reinterpret_cast<const char*>(&name_len), 1);
        file_.write(info.name.c_str(), name_len);
        file_.write(reinterpret_cast<const char*>(&size), 4);
    }

    // Full-state snapshot as SPAWN records.
    DeltaSet snap = capture_world_snapshot(world);
    std::vector<uint8_t> buf;
    snap.serialize(world.codec_registry(), buf);
    uint32_t snap_len = static_cast<uint32_t>(buf.size());
    file_.write(reinterpret_cast<const char*>(&snap_len), 4);
    file_.write(reinterpret_cast<const char*>(buf.data()), buf.size());

    // Tick count placeholder (patched on close).
    ticks_pos_ = file_.tellp();
    uint32_t zero = 0;
    file_.write(reinterpret_cast<const char*>(&zero), 4);
}

void ReplayRecorder::record_tick(ecs::World& world) {
    if (!file_.is_open() || !recording_) return;

    uint64_t since = last_tick_;
    last_tick_ = world.current_tick();

    DeltaSet set = capture_tick_delta(world, since);

    std::vector<uint8_t> buf;
    set.serialize(world.codec_registry(), buf);
    uint32_t len = static_cast<uint32_t>(buf.size());
    file_.write(reinterpret_cast<const char*>(&len), 4);
    file_.write(reinterpret_cast<const char*>(buf.data()), buf.size());
    ++ticks_written_;
}

void ReplayRecorder::close() {
    if (file_.is_open()) {
        if (recording_) {
            file_.seekp(ticks_pos_);
            uint32_t n = static_cast<uint32_t>(ticks_written_);
            file_.write(reinterpret_cast<const char*>(&n), 4);
        }
        file_.close();
    }
}

// ── ReplayPlayer ──────────────────────────────────────────────

ReplayPlayer::~ReplayPlayer() {
    if (file_.is_open()) file_.close();
}

bool ReplayPlayer::open(const std::string& path) {
    if (opened_) return false;
    file_.open(path, std::ios::binary);
    if (!file_.is_open()) return false;

    uint32_t magic = 0, version = 0;
    file_.read(reinterpret_cast<char*>(&magic), 4);
    file_.read(reinterpret_cast<char*>(&version), 4);
    if (magic != kReplayMagic || version < 1 || version > 2) {
        file_.close();
        return false;
    }
    file_.read(reinterpret_cast<char*>(&start_tick_), 8);

    uint32_t num_comps = 0;
    file_.read(reinterpret_cast<char*>(&num_comps), 4);
    for (uint32_t i = 0; i < num_comps; ++i) {
        ComponentDef def;
        file_.read(reinterpret_cast<char*>(&def.id), 1);
        file_.read(reinterpret_cast<char*>(&def.codec), 1);
        uint8_t name_len = 0;
        file_.read(reinterpret_cast<char*>(&name_len), 1);
        def.name.resize(name_len);
        file_.read(&def.name[0], name_len);
        file_.read(reinterpret_cast<char*>(&def.size), 4);
        comps_.push_back(def);
        codecs_.set_codec(def.id, static_cast<CodecID>(def.codec));
    }

    uint32_t snap_len = 0;
    file_.read(reinterpret_cast<char*>(&snap_len), 4);
    snap_buf_.resize(snap_len);
    file_.read(reinterpret_cast<char*>(snap_buf_.data()), snap_len);

    uint32_t num_ticks = 0;
    file_.read(reinterpret_cast<char*>(&num_ticks), 4);
    tick_bufs_.resize(num_ticks);
    for (uint32_t i = 0; i < num_ticks; ++i) {
        uint32_t len = 0;
        file_.read(reinterpret_cast<char*>(&len), 4);
        tick_bufs_[i].resize(len);
        file_.read(reinterpret_cast<char*>(tick_bufs_[i].data()), len);
    }

    opened_ = true;
    return true;
}

void ReplayPlayer::load_components(ecs::World& world) const {
    for (const auto& def : comps_) {
        world.get_store()->register_component(def.name, def.size);
        world.codec_registry().set_codec(def.id, static_cast<CodecID>(def.codec));
    }
}

bool ReplayPlayer::step(ecs::World& world) {
    if (!opened_) return false;

    if (!snapshot_applied_) {
        snapshot_applied_ = true;
        DeltaSet snap;
        if (snap.deserialize(codecs_, snap_buf_.data(), snap_buf_.size())) {
            snap.apply(world);
            world.advance_to(start_tick_);
        }
        return true;
    }

    if (tick_index_ >= tick_bufs_.size()) return false;

    DeltaSet set;
    if (set.deserialize(codecs_, tick_bufs_[tick_index_].data(), tick_bufs_[tick_index_].size())) {
        set.apply(world);
    }
    ++tick_index_;
    world.advance_tick();
    return true;
}

// ── Save compaction (delta folding, #7) ───────────────────────

namespace {

// Pliega la cadena snapshot + ticks en un ÚNICO DeltaSet del estado final:
// last-write-wins por (entity, comp); los DESPAWN eliminan todo el estado
// previo del entity; las aristas se re-emiten en su estado final.
class DeltaFolder {
public:
    void consume(const DeltaSet& set) {
        set.for_each_record([&](const DeltaRecord& r) { consume(r); });
    }

    DeltaSet finish(uint64_t base_tick) {
        DeltaSet folded(base_tick);
        folded.set_end_tick(base_tick);

        for (uint32_t e : entity_order_) {
            const auto it = entities_.find(e);
            if (it == entities_.end() || it->second.dead) continue;
            folded.add_spawn(e);
            for (const auto& [comp, data] : it->second.comps) {
                folded.add_spawn(e, comp, data.data(), data.size());
            }
        }

        for (uint32_t e : despawn_order_) {
            auto it = entities_.find(e);
            if (it == entities_.end() || !it->second.dead) continue;
            folded.add_despawn(e);
        }

        for (const auto& [src, kind, dst, payload] : edges_) {
            folded.add_relation(src, kind, dst, payload);
        }
        return folded;
    }

private:
    struct EntityState {
        std::vector<std::pair<ecs::ComponentID, std::vector<uint8_t>>> comps;
        bool dead = false;
    };

    void consume(const DeltaRecord& r) {
        switch (r.op) {
            case DeltaOp::DESPAWN:
                entities_[r.entity].dead = true;
                despawn_order_.push_back(r.entity);
                break;
            case DeltaOp::SPAWN:
            case DeltaOp::UPDATE: {
                EntityState& st = entities_[r.entity];
                if (r.op == DeltaOp::SPAWN) {
                    st.dead = false; // resurrección: un spawn revive al entity
                } else if (st.dead) {
                    break; // update de un entity ya muerto: no importa
                }
                if (r.op == DeltaOp::SPAWN && r.comp_id == 255) break; // spawn desnudo
                if (st.comps.empty() && entity_seen_.emplace(r.entity, true).second) {
                    entity_order_.push_back(r.entity);
                }
                bool replaced = false;
                for (auto& [comp, data] : st.comps) {
                    if (comp == r.comp_id) {
                        data = r.data;
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    st.comps.emplace_back(r.comp_id, r.data);
                }
                break;
            }
            case DeltaOp::RELATION: {
                // data = [u8 action][u32 dst][u8 payload_len][payload]
                if (r.data.size() < 6) break;
                uint8_t action = r.data[0];
                uint32_t dst = 0;
                std::memcpy(&dst, r.data.data() + 1, 4);
                uint8_t payload_len = r.data[5];
                ecs::RelationPayload payload;
                if (payload_len > 0 && r.data.size() >= 6 + payload_len) {
                    payload.raw = 0;
                    std::memcpy(&payload.raw, r.data.data() + 6, payload_len);
                }
                auto it = std::find_if(edges_.begin(), edges_.end(),
                    [&](const EdgeRef& e) { return e.src == r.entity && e.kind == r.comp_id && e.dst == dst; });
                if (action == kRelationActionAdd) {
                    if (it == edges_.end()) edges_.push_back({r.entity, r.comp_id, dst, payload});
                    else it->payload = payload;
                } else {
                    if (it != edges_.end()) edges_.erase(it);
                }
                break;
            }
            default:
                break;
        }
    }

    struct EdgeRef {
        uint32_t src;
        ecs::RelationKind kind;
        uint32_t dst;
        ecs::RelationPayload payload;
    };

    std::unordered_map<uint32_t, EntityState> entities_;
    std::unordered_map<uint32_t, bool> entity_seen_;
    std::vector<uint32_t> entity_order_;
    std::vector<uint32_t> despawn_order_;
    std::vector<EdgeRef> edges_;
};

} // namespace

bool fold_replay_file(const std::string& in_path, const std::string& out_path) {
    ReplayPlayer player;
    if (!player.open(in_path)) return false;

    // Componentes y codecs viajan al archivo compacto.
    std::vector<ReplayPlayer::ComponentDef> comps = player.comps_;
    CodecRegistry codecs = player.codecs_;

    // Aplicar snapshot + cadena de deltas en orden al folder.
    DeltaFolder folder;
    DeltaSet snap;
    if (!snap.deserialize(codecs, player.snap_buf_.data(), player.snap_buf_.size())) {
        return false;
    }
    folder.consume(snap);
    uint64_t final_tick = player.start_tick_;
    for (const auto& buf : player.tick_bufs_) {
        DeltaSet set;
        if (!set.deserialize(codecs, buf.data(), buf.size())) {
            return false;
        }
        folder.consume(set);
        final_tick = set.end_tick();
    }

    DeltaSet folded = folder.finish(final_tick);

    // Escribir archivo compacto: tabla de componentes + snapshot final + 0 ticks.
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    uint32_t magic = ReplayPlayer::kReplayMagic;
    uint32_t version = 2;
    uint64_t start_tick = final_tick;
    out.write(reinterpret_cast<const char*>(&magic), 4);
    out.write(reinterpret_cast<const char*>(&version), 4);
    out.write(reinterpret_cast<const char*>(&start_tick), 8);

    uint32_t num_comps = static_cast<uint32_t>(comps.size());
    out.write(reinterpret_cast<const char*>(&num_comps), 4);
    for (const auto& def : comps) {
        uint8_t id = def.id;
        uint8_t codec = def.codec;
        uint8_t name_len = static_cast<uint8_t>(def.name.size());
        uint32_t size = def.size;
        out.write(reinterpret_cast<const char*>(&id), 1);
        out.write(reinterpret_cast<const char*>(&codec), 1);
        out.write(reinterpret_cast<const char*>(&name_len), 1);
        out.write(def.name.c_str(), name_len);
        out.write(reinterpret_cast<const char*>(&size), 4);
    }

    std::vector<uint8_t> buf;
    folded.serialize(codecs, buf);
    uint32_t snap_len = static_cast<uint32_t>(buf.size());
    out.write(reinterpret_cast<const char*>(&snap_len), 4);
    out.write(reinterpret_cast<const char*>(buf.data()), buf.size());

    uint32_t zero_ticks = 0;
    out.write(reinterpret_cast<const char*>(&zero_ticks), 4);
    return true;
}

} // namespace delta
} // namespace fluxdb
