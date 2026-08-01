// Agrupación por tipo → Agrupación por tipo (como Bevy/Unity DOTS), etc
// Debemos crear una estructura de datos que permita almacenar los componentes de manera contigua en memoria.
// Esto permite un mejor rendimiento debido a la localidad de caché.
//
// (#8) El storage es de páginas de chunks con copy-on-write: cada componente
// se guarda en páginas de Archetype::PAGE_ROWS filas. Los snapshots de
// rollback comparten las páginas por shared_ptr; un write a una página
// compartida la clona primero (ensure_owned). "Rollback" = repuntar los
// punteros activos a la versión histórica.

#include "../headers/ecs.h"
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>

namespace fluxdb {
namespace ecs {

Archetype::Archetype(ArchetypeSignature sig, const ComponentStore& store)
    : signature_(sig), store_(&store) {
    // Para cada bit de la firma, registrar su tamaño y preparar las páginas
    for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
        if (sig.test(i)) {
            ComponentID id = static_cast<ComponentID>(i);
            size_t size = store.get_info(id).size;
            component_sizes_[id] = size;
            pages_[id]; // vector de páginas vacío (crecimiento perezoso)
            last_write_ticks_[id] = std::vector<uint32_t>();
            dirty_chunks_[id]; // tracker por componente
        }
    }
}

uint8_t* Archetype::ensure_owned(ComponentID comp_id, size_t chunk) {
    auto& vec = pages_[comp_id];
    while (vec.size() <= chunk) {
        vec.push_back(ChunkPage{});
    }
    ChunkPage& page = vec[chunk];
    size_t bytes = PAGE_ROWS * component_sizes_[comp_id];
    if (!page.data) {
        page.data = std::shared_ptr<uint8_t[]>(new uint8_t[bytes]()); // zero-init (C++17)
    } else if (page.data.use_count() > 1) {
        // Copia-on-write: la página está compartida con snapshots de
        // rollback (#8) → clonar antes de escribir.
        auto copy = std::shared_ptr<uint8_t[]>(new uint8_t[bytes]);
        std::memcpy(copy.get(), page.data.get(), bytes);
        page.data = std::move(copy);
    }
    return page.data.get();
}

size_t Archetype::add_entity(Entity entity, uint64_t tick) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    size_t row = num_entities_;
    entities_.push_back(entity);

    // Expandir: asegurar la página del chunk de la fila y zero-fill el slot
    for (auto& [id, vec] : pages_) {
        size_t size = component_sizes_[id];
        size_t chunk = row / PAGE_ROWS;
        uint8_t* page = ensure_owned(id, chunk);
        std::memset(page + (row % PAGE_ROWS) * size, 0, size);
    }
    for (auto& [id, ticks] : last_write_ticks_) {
        ticks.push_back(0);
    }
    if (tick > 0) {
        for (auto& [id, ring] : dirty_rings_) {
            ring.mark(tick);
        }
        for (auto& [id, tracker] : dirty_chunks_) {
            tracker.mark_all(tick); // cambio estructural: todos los chunks sucios
        }
    }

    num_entities_++;
    return row;
}

Entity Archetype::remove_entity(size_t row, uint64_t tick) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    
    if (row >= num_entities_) return UINT32_MAX;

    size_t last_row = num_entities_ - 1;
    Entity moved_entity = UINT32_MAX;

    // Swap and pop
    if (row != last_row) {
        // Mover Entity ID
        entities_[row] = entities_[last_row];
        moved_entity = entities_[row];

        // Mover Data (COW: clona las páginas destino si están compartidas)
        for (auto& [id, vec] : pages_) {
            size_t size = component_sizes_[id];
            uint8_t* dst_page = ensure_owned(id, row / PAGE_ROWS);
            uint8_t* src_page = ensure_owned(id, last_row / PAGE_ROWS);
            uint8_t* dst = dst_page + (row % PAGE_ROWS) * size;
            uint8_t* src = src_page + (last_row % PAGE_ROWS) * size;
            std::memcpy(dst, src, size);
        }
        for (auto& [id, ticks] : last_write_ticks_) {
            ticks[row] = ticks[last_row];
            // Precisión de chunk: la entidad movida trae su tick de último
            // write; si lo tiene, su chunk destino queda marcado para que
            // for_each_changed no lo pierda tras el swap (despawn sin tick).
            if (ticks[row] > 0) {
                dirty_chunks_[id].mark(row, ticks[row]);
            }
        }
    }

    entities_.pop_back();

    // Las páginas NO se encogen (granularidad de chunk): el slot queda
    // inerte (num_entities_ decrementado) y el siguiente add_entity lo
    // zero-fillea.
    for (auto& [id, ticks] : last_write_ticks_) {
        ticks.pop_back();
    }
    if (tick > 0) {
        for (auto& [id, ring] : dirty_rings_) {
            ring.mark(tick);
        }
        for (auto& [id, tracker] : dirty_chunks_) {
            tracker.mark_all(tick); // cambio estructural: todos los chunks sucios
        }
    }

    num_entities_--;
    return moved_entity;
}

void Archetype::set_component_data(size_t row, ComponentID comp_id, const void* data, Entity entity, HistoryManager* history, uint64_t tick) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = pages_.find(comp_id);
    if (it != pages_.end()) {
        size_t size = component_sizes_[comp_id];
        size_t chunk = row / PAGE_ROWS;
        uint8_t* page = ensure_owned(comp_id, chunk); // COW si está compartida
        uint8_t* dst = page + (row % PAGE_ROWS) * size;
        
        // Push current memory state to History (Time-Travel) before overwriting
        if (history && entity != UINT32_MAX) {
            history->record_change(entity, comp_id, dst, size);
        }

        std::memcpy(dst, data, size);

        // (#1) Heurística: contador de acceso (solo con profiling activo).
        if (store_ && store_->access_profiling()) {
            ++access_counts_[comp_id];
        }

        // Temporal Component Versioning (#4): sella el write
        if (tick > 0) {
            last_write_ticks_[comp_id][row] = static_cast<uint32_t>(tick);
            dirty_rings_[comp_id].mark(tick);
            dirty_chunks_[comp_id].mark(row, tick); // ring del chunk de la fila
        }
    }
}

void* Archetype::get_component_data(size_t row, ComponentID comp_id) {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = pages_.find(comp_id);
    if (it != pages_.end()) {
        // (#1) Heurística: contador de acceso (solo con profiling activo).
        if (store_ && store_->access_profiling()) {
            ++access_counts_[comp_id];
        }
        size_t size = component_sizes_[comp_id];
        size_t chunk = row / PAGE_ROWS;
        const std::vector<ChunkPage>& vec = it->second;
        if (vec.empty() || chunk >= vec.size()) return nullptr;
        return vec[chunk].data.get() + (row % PAGE_ROWS) * size;
    }
    return nullptr;
}

bool Archetype::has_component(ComponentID comp_id) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return pages_.find(comp_id) != pages_.end();
}

bool Archetype::has_any_write_since(ComponentID comp_id, uint64_t since_tick) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = dirty_rings_.find(comp_id);
    if (it == dirty_rings_.end()) {
        return false;
    }
    return it->second.has_writes_since(since_tick);
}

bool Archetype::chunk_has_writes_since(ComponentID comp_id, size_t chunk_idx, uint64_t since_tick) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = dirty_chunks_.find(comp_id);
    if (it == dirty_chunks_.end()) {
        return false;
    }
    return it->second.chunk_has_writes_since(chunk_idx, since_tick);
}

size_t Archetype::chunk_count(ComponentID comp_id) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    if (dirty_chunks_.find(comp_id) == dirty_chunks_.end()) {
        return 0;
    }
    return (num_entities_ + ChunkedDirtyTracker::CHUNK_SIZE - 1) / ChunkedDirtyTracker::CHUNK_SIZE;
}

uint32_t Archetype::last_write_tick(size_t row, ComponentID comp_id) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = last_write_ticks_.find(comp_id);
    if (it == last_write_ticks_.end() || row >= it->second.size()) {
        return 0;
    }
    return it->second[row];
}

const Entity* Archetype::get_entities_ptr() const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    return entities_.empty() ? nullptr : entities_.data();
}

const uint32_t* Archetype::get_last_write_ticks_ptr(ComponentID comp_id) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = last_write_ticks_.find(comp_id);
    if (it == last_write_ticks_.end()) {
        return nullptr;
    }
    return it->second.data();
}

// ── COW structural snapshot (#8) ──────────────────────────────

void Archetype::snapshot_pages(ChunkPageSnapshot::ArchetypeState& out) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    out.entities = entities_; // copia barata: 4B por entidad
    out.num_entities = static_cast<uint32_t>(num_entities_);
    for (const auto& [id, ticks] : last_write_ticks_) {
        out.last_write_ticks[id] = ticks;
    }
    for (const auto& [id, vec] : pages_) {
        auto& dst = out.pages[id];
        dst.resize(vec.size());
        for (size_t i = 0; i < vec.size(); ++i) {
            dst[i] = vec[i].data; // COMPARTIR: refcount++
        }
    }
}

void Archetype::restore_pages(const ChunkPageSnapshot::ArchetypeState& in) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    entities_ = in.entities;
    num_entities_ = in.num_entities;

    for (auto& [id, vec] : pages_) {
        vec.clear();
        auto it = in.pages.find(id);
        if (it != in.pages.end()) {
            vec.resize(it->second.size());
            for (size_t i = 0; i < it->second.size(); ++i) {
                vec[i].data = it->second[i]; // repuntar: refcount++
            }
        }
    }
    for (auto& [id, ticks] : last_write_ticks_) {
        auto it = in.last_write_ticks.find(id);
        if (it != in.last_write_ticks.end()) {
            ticks = it->second;
        } else {
            ticks.assign(num_entities_, 0);
        }
        // Restaurar la metadata de versionado (#4): rings grueso/fino por
        // fila, para que los diffs posteriores al rollback sigan exactos.
        dirty_rings_[id];
        for (size_t row = 0; row < ticks.size(); ++row) {
            if (ticks[row] > 0) {
                dirty_rings_[id].mark(ticks[row]);
                dirty_chunks_[id].mark(row, ticks[row]);
            }
        }
    }
}

size_t Archetype::page_share_count(ComponentID comp_id, size_t chunk) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = pages_.find(comp_id);
    if (it == pages_.end() || chunk >= it->second.size()) return 0;
    if (!it->second[chunk].data) return 0;
    return it->second[chunk].data.use_count();
}

// ── Hot-Reload Component Schemas (#30) ────────────────────────

size_t Archetype::migrate_component_layout(ComponentID comp_id, size_t new_size,
                                           const FieldMap* field_map, size_t num_fields,
                                           uint8_t default_fill) {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);

    auto sit = component_sizes_.find(comp_id);
    if (sit == component_sizes_.end() || sit->second == new_size) return 0;
    size_t old_size = sit->second;

    auto pit = pages_.find(comp_id);
    if (pit == pages_.end()) return 0;
    std::vector<ChunkPage>& chunks = pit->second;

    const size_t rows_per_chunk = PAGE_ROWS;
    const size_t new_page_bytes = rows_per_chunk * new_size;
    const size_t old_page_bytes = rows_per_chunk * old_size;

    size_t migrated = 0;
    size_t num_chunks = (num_entities_ + rows_per_chunk - 1) / rows_per_chunk;
    for (size_t c = 0; c < num_chunks && c < chunks.size(); ++c) {
        ChunkPage& old_page = chunks[c];
        std::shared_ptr<uint8_t[]> new_data(new uint8_t[new_page_bytes]);
        // Relleno por defecto (campos nuevos quedan con default_fill).
        std::memset(new_data.get(), default_fill, new_page_bytes);

        size_t rows = std::min(rows_per_chunk, num_entities_ - c * rows_per_chunk);
        const uint8_t* src = old_page.data.get();
        if (!src) continue;
        for (size_t r = 0; r < rows; ++r) {
            const uint8_t* in_row = src + r * old_size;
            uint8_t* out_row = new_data.get() + r * new_size;
            for (size_t f = 0; f < num_fields; ++f) {
                const FieldMap& fm = field_map[f];
                if (fm.old_offset + fm.length <= old_size &&
                    fm.new_offset + fm.length <= new_size) {
                    std::memcpy(out_row + fm.new_offset, in_row + fm.old_offset, fm.length);
                }
            }
            ++migrated;
        }
        // Swap atómico de la página.
        old_page.data = std::move(new_data);
    }

    sit->second = new_size;
    return migrated;
}

// ── Hot/Cold Archetype Splitting (#1) ─────────────────────────

std::vector<ComponentID> Archetype::components_in_tier(uint8_t tier_mask) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    std::vector<ComponentID> out;
    // Orden canónico: ComponentID ascendente (independiente del unordered_map).
    for (size_t i = 0; i < MAX_COMPONENTS; ++i) {
        if (!signature_.test(i)) continue;
        ComponentID id = static_cast<ComponentID>(i);
        if (store_ && tier_in_mask(store_->get_tier(id), tier_mask)) {
            out.push_back(id);
        }
    }
    return out;
}

size_t Archetype::prefetch_components(const std::vector<ComponentID>& comps) {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    size_t touched = 0;
    for (ComponentID comp : comps) {
        auto it = pages_.find(comp);
        if (it == pages_.end()) continue;
        for (const ChunkPage& page : it->second) {
            if (!page.data) continue;
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(page.data.get(), 0, 3);
#endif
            // Touch real: mete la primera línea de caché de la página en L1.
            volatile uint8_t touch = page.data.get()[0];
            (void)touch;
            ++touched;
        }
    }
    return touched;
}

size_t Archetype::page_count(ComponentID comp_id) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = pages_.find(comp_id);
    if (it == pages_.end()) return 0;
    size_t n = 0;
    for (const ChunkPage& page : it->second) {
        if (page.data) ++n;
    }
    return n;
}

uint64_t Archetype::access_count(ComponentID comp_id) const {
    std::shared_lock<std::shared_mutex> lock(rw_mutex_);
    auto it = access_counts_.find(comp_id);
    if (it == access_counts_.end()) return 0;
    return it->second;
}

void Archetype::decay_access_counts() {
    std::unique_lock<std::shared_mutex> lock(rw_mutex_);
    for (auto it = access_counts_.begin(); it != access_counts_.end();) {
        // Decay exponencial half-life 1 tick: steady-state = accesos/tick × 2
        // (la heurística de #1 mide TASA, no acumulado).
        uint64_t c = it->second / 2;
        if (c == 0) {
            it = access_counts_.erase(it);
        } else {
            it->second = c;
            ++it;
        }
    }
}

} // namespace ecs
} // namespace fluxdb
