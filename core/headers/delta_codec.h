#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────
//  Per-component delta strategies (#7)
// ─────────────────────────────────────────────────────────────
// Components opt into compression strategies either at runtime via
// World::set_codec(comp_id, CodecID) or at compile time via the
// DeltaCodec<T> trait: specialize it for a C++ type and register the
// component with World::set_codec<T>(comp_id).

namespace fluxdb {
namespace delta {

enum class CodecID : uint8_t {
    RAW = 0,              // memcpy passthrough (full precision)
    QUANTIZED_FLOAT = 1,  // float arrays -> [float scale][int16 values...]
    RLE = 2,              // byte arrays -> runs [uint8 value][uint32 count]
    BITPACK = 3,          // integer arrays -> [u8 val_size][u8 bit_width][u32 n][bits]
    SECTOR_POS = 4,       // SectorPos (#19) -> [int32 sx][int32 sy][int32 sz][float scale][int16 ox oy oz]
};

// Compile-time default: RAW. Specialize per type, e.g.:
//   template <> struct DeltaCodec<Position> {
//       static constexpr CodecID id() { return CodecID::QUANTIZED_FLOAT; }
//   };
//   template <> struct DeltaCodec<SectorPos> {
//       static constexpr CodecID id() { return CodecID::SECTOR_POS; }
//   };
template <typename T>
struct DeltaCodec {
    static constexpr CodecID id() { return CodecID::RAW; }
};

} // namespace delta
} // namespace fluxdb
