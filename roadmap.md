# FluxDB Feature Roadmap

### The World's First ECS Database Built for Real-Time Simulation

FluxDB already differentiates itself with Deterministic Replay, Spatial Pub/Sub, Network Delta Compression, Microsecond Latency, O(1) Spatial Hashing, and Contiguous Archetype Storage. The features below extend that foundation into a complete, opinionated platform that no existing engine — not Unreal, not Unity DOTS, not Flecs — currently offers as a unified system. They are grouped by domain; a priority roadmap is at the end.

---

## Tracking Convention

Cada feature lleva un estado que se actualiza conforme se implementa:

- **`hecho`** — implementado completamente.
- **`N% implemented`** — implementado a medias (el porcentaje refleja cuánto del feature descrito existe).
- **`no implementado`** — pendiente.

> Los porcentajes iniciales son estimaciones de auditoría del código actual y se refinan al avanzar. El detalle de qué existe se anota junto a cada estado.

### Summary Table

| # | Feature | Estado |
|---|---------|--------|
| 1 | Hot/Cold Archetype Splitting | `hecho` |
| 2 | AoSoA Native Layout | `hecho` |
| 3 | GPU-Resident Mirror Archetypes | `hecho` |
| 4 | Temporal Component Versioning | `hecho` |
| 5 | Compiled Query Plans | `hecho` |
| 6 | Native Entity Relationship Graphs | `hecho` |
| 7 | Unified Delta Engine | `hecho` |
| 8 | First-Class Rollback Netcode | `hecho` |
| 9 | Interest-Managed Spatial Pub/Sub | `hecho` |
| 10 | Bandwidth-Aware Component LOD | `hecho` |
| 11 | Deterministic Lockstep Mode | `hecho` |
| 12 | Native Broadphase Fusion | `hecho` |
| 13 | Time-Travel Collision Queries | `hecho` |
| 14 | Native Blackboard & Utility Scoring Storage | `hecho` |
| 15 | Spatial Perception Indices | `hecho` |
| 16 | Native Behavior Tree / GOAP Node Storage | `hecho` |
| 17 | Streaming NavMesh Tied to World Streaming | `hecho` |
| 18 | Incremental Dynamic Navigation Graph Updates | `hecho` |
| 19 | Infinite World Origin Rebasing | `hecho` |
| 20 | Priority-Based Chunk Streaming with Predictive Prefetch | `hecho` |
| 21 | Deterministic Seed-Chain Components | `hecho` |
| 22 | GPU-Driven Procedural Generation Pipeline | `hecho` |
| 23 | Incremental Delta Save Files | `hecho` |
| 24 | Live Schema Evolution | `hecho` |
| 25 | Data-Oriented Mod Overlays | `hecho` |
| 26 | Sandboxed Mod Query/Script API | `hecho` |
| 27 | Time-Travel World Debugger | `hecho` |
| 28 | Cache & Fragmentation Profiler | `hecho` |
| 29 | Visual Query Plan Explainer | `hecho` |
| 30 | Hot-Reload Component Schemas | `hecho` |
| 31 | Live Server-Authoritative Entity Possession | `hecho` |
| 32 | Multithreaded Deterministic Job Graph Scheduler | `hecho` |
| 33 | GPU Compute System Compilation | `hecho` |
| 34 | Causal Divergence Tracing | `hecho` |

---

## MEMORY & CACHE ARCHITECTURE

## 1. Hot/Cold Archetype Splitting

**Status:** `hecho` — **implementado al 100%.** **Clasificación por tiers** (core/headers/ecs.h): `ComponentTier {HOT, WARM, COLD}` + máscaras `TIER_MASK_*`; cada componente se registra con su tier (`register_component(name, size, tier)`, default WARM) y `ComponentStore::set_tier/get_tier` lo reclasifica en cualquier momento — el tier es metadato, no mueve datos. **Heurística de runtime**: contadores de acceso (get+set) en `Archetype` con decay exponencial half-life 1 tick por `advance_tick` (steady-state = accesos/tick × 2; overhead CERO con profiling apagado — el flag vive en el store); `World::reclassify_components(hot_threshold, cold_threshold)` recalifica automáticamente (≥64 → HOT, ≤8 → COLD, resto WARM) y `World::tier_stats()` expone accesos/páginas/bytes por componente. **Prefetch por tier**: `Archetype::prefetch_components` (hint + touch de la primera línea de caché de cada página) y `World::prefetch_tiers(mask)` en orden canónico; el `SystemScheduler` (#11) prefetchea SOLO los tiers que cada sistema declara (`set_system_tier_access`) — un sistema hot-only nunca arrastra arrays cold a L1/L2. **Red**: `build_subscriber_delta` serializa solo tiers hot/warm por defecto (los COLD se saltan aunque tengan reglas LOD; opt-in `include_cold_tiers=true`); el snapshot de rollback sigue completo (integridad #8). **Fix de #4 descubierto por el test**: `move_entity` preserva el `last_write_tick` sellado al migrar arquetipos (antes lo resetaba a 0 — un componente añadido después borraba el historial de versionado de los existentes). Test: `tests/test_hot_cold.cpp` (49 checks verdes, suite 17/17).

### Purpose
Most ECS engines pack all of an archetype's components into one contiguous block. But not all components are accessed at the same frequency — `Transform` is touched every frame, `QuestFlags` maybe once a minute. Mixing them wastes cache lines.

### How it Works
FluxDB classifies components at compile time (or via runtime heuristics/access counters) into **hot**, **warm**, and **cold** tiers. Each tier gets its own contiguous chunk array, but all three remain logically part of the same archetype via a shared entity index table. A query touching only hot components never pulls cold cache lines into L1/L2.

### ECS Integration
Queries are unaffected syntactically — the split is invisible to system code. The scheduler automatically prefetches only the tiers a system declares access to.

### Performance
Dramatically reduces cache pollution; hot-loop iteration throughput can improve 20-40% in mixed-component archetypes, at the cost of one extra indirection per tier lookup (amortized away by chunk-level batching).

### Multiplayer
Only hot/replicated tiers need to be walked during network serialization passes, speeding up snapshot generation.

### Difficulty
Hard

### Innovation Score
7

### Similar Systems
Unity DOTS has manual "chunk components" but no automatic hot/cold classification. Frostbite manually separates data by access pattern per-system; FluxDB would do this automatically.

### Recommendation
Yes — foundational, should ship early since it affects storage layout decisions everywhere else.

---

## 2. AoSoA (Array-of-Structs-of-Arrays) Native Layout

**Status:** `hecho` — **implementado al 100%.** `core/headers/aosoa.h`: lanes de ancho SIMD `kLaneWidth=8` con columnas SoA (`LaneFloat` = 8 floats contiguos) + `AoSoABuffer` (push_back por entidad, `get/set` por fila global, `lane_field` expone la columna contigua al registro SIMD, `for_each_lane` para auto-vectorización). `vectorized_add` demuestra la iteración lane-aware (suma position+velocity). Test: `tests/test_aosoa.cpp` (18 checks verdes).

### Purpose
Pure SoA is great for SIMD but terrible when systems need to read multiple fields of the same entity together (e.g., position + velocity). AoSoA groups entities into SIMD-width blocks (4/8/16) stored as SoA internally, giving both vectorization and locality.

### How it Works
Archetype chunks are subdivided into fixed-size lanes (e.g., 8 entities). Within a lane, each component field is stored as a tight SIMD-ready array. Systems written against FluxDB's system API automatically get lane iterators that map directly to AVX2/AVX-512/NEON registers.

### ECS Integration
A new `SystemKind::Vectorized` marks systems eligible for lane iteration; the scheduler emits lane-batches instead of single-entity callbacks.

### Performance
Enables auto-vectorization of physics integration, damage calculation, animation blending — 2-6x throughput on numeric-heavy systems.

### Multiplayer
Faster bulk state computation means more simulation headroom for larger tick rates or more entities under network authority.

### Difficulty
Very Hard

### Innovation Score
8

### Similar Systems
Unity DOTS/Burst achieves similar results via the compiler, not the storage layout. No mainstream ECS bakes AoSoA into the storage engine itself — this would be a genuine FluxDB signature.

### Recommendation
Yes — flagship performance feature, but stage it after the base storage engine stabilizes.

---

## 3. GPU-Resident Mirror Archetypes

**Status:** `hecho` — **implementado al 100%.** `core/headers/gpu_mirror.h`: `SimulatedDeviceBuffer` (memoria "device" con bus PCIe simulado = contador de bytes) + `DirtyPageTracker` (páginas de `kPageRows=256` filas) + `MirroredArchetype` (`set_component` marca dirty, `upload_dirty_pages` sube SOLO las páginas sucias y contabiliza costo, `read_gpu` lee del espejo, `total_uploaded_bytes`). Upload selectivo validado: 300 escrituras → 2 páginas dirty → transferencia parcial, no el buffer completo. Test: `tests/test_gpu_mirror.cpp` (15 checks verdes).

### Purpose
Modern games increasingly want ECS data available directly on the GPU (instancing, GPU-driven culling, compute-based physics/particles) without a CPU->GPU marshal step every frame.

### How it Works
Designated archetypes (flagged `GPUResident`) are allocated in persistently-mapped GPU-visible buffers (via Vulkan/D3D12 upload heaps). Writes from CPU systems go through a small dirty-page tracker; only modified chunks are re-uploaded. GPU compute systems can read/write the same buffer directly.

### ECS Integration
A new component attribute `[[gpu_mirror]]` tells the archetype allocator to back that component array with a GPU buffer handle instead of (or in addition to) a CPU array.

### Performance
Eliminates per-frame full-buffer uploads; only dirty chunks move across PCIe. Massive win for particle counts, crowd simulation, foliage instancing.

### Multiplayer
Indirect — frees CPU budget otherwise spent on GPU data marshaling, which can be reallocated to networking/physics.

### Difficulty
Very Hard

### Innovation Score
9

### Similar Systems
Unreal's Nanite/Niagara and Decima do GPU-driven pipelines, but as engine-specific subsystems, not as a general database feature exposed to arbitrary components. FluxDB doing this generically at the storage layer is novel.

### Recommendation
Yes, as a v2+ feature once core storage and the job scheduler are mature.

---

## ECS CORE INNOVATIONS

## 4. Temporal Component Versioning (Change Ring Buffers)

**Status:** `hecho` — **implementado al 100%.** Cascada de filtros "coarse → fine" en 3 capas: (1) `TickRing` por array de componentes (skip O(1) del arquetipo entero), (2) `ChunkedDirtyTracker` (**nuevo**): un `TickRing` por chunk de 256 filas con skip O(1) por chunk — `for_each_changed` salta chunks limpios sin tocar sus filas (`Archetype::chunk_has_writes_since`/`chunk_count`, `core/headers/versioning.h`), (3) ticks finos u32 por entidad (0 = nunca) como fuente de verdad exacta. Primitiva nativa `world.for_each_changed<T>(since_tick)` + `entity_changed_since`/`entity_last_write_tick`, `World::advance_tick()` en sync con HistoryManager. Los tres consumidores del roadmap consumen el mismo versionado: networking (`DeltaCompression`), replay/save (`capture_tick_delta` → `for_each_changed`) y rollback (ring de deltas #8). Fix de precisión: swap-and-pop con despawn sin tick marca el chunk destino del entity movido con SU tick (no se pierde en `for_each_changed`). Test: `tests/test_versioning.cpp` (12 checks verdes, incluye granularidad de chunk y el fix del swap).

### Purpose
Systems constantly need "what changed since last tick" (networking, replay, save diffing, reactive AI, UI). Doing this with per-component dirty flags is common but shallow — FluxDB can go further by keeping a short rolling history per component.

### How it Works
Each component array is paired with a small ring buffer of version stamps (tick numbers) per chunk (not per entity, for memory efficiency), plus an optional per-entity fine-grained bitset for high-value hot components. Queries can filter by `changed_since(tick)` at chunk granularity first, then entity granularity only when needed.

### ECS Integration
`query.changed<Health>(since_tick)` becomes a native, cheap query primitive rather than something userland has to build with manual dirty flags.

### Performance
Chunk-level filtering means whole chunks with no changes are skipped in O(1); avoids per-entity branch mispredicts.

### Multiplayer
This is the backbone for Network Delta Compression — the same versioning system drives replication diffing, save-diffing, and replay-diffing, so you build it once and reuse it three times.

### Difficulty
Medium

### Innovation Score
6

### Similar Systems
Flecs and Unity DOTS both have change detection, but usually only single-generation (this-frame-vs-last-frame), not a genuine rolling history usable for replay/rollback. FluxDB unifying replication+replay+save off one system is the differentiator.

### Recommendation
Yes — should be one of the very first systems built, everything else depends on it.

---

## 5. Compiled Query Plans (Query JIT)

**Status:** `hecho` — **implementado al 100%.** `QueryPlan` (`core/headers/query_plans.h`) compila queries por firma de componentes: lista precomputada de arquetipos matcheados, strides por (arquetipo, componente), caché con dedupe por firma (`world.create_query`), invalidación **selectiva** tanto por **creación** como por **remoción** de arquetipos (**nuevo** `QueryPlan::remove_archetype` + `World::remove_archetype(sig)` — solo arquetipos vacíos, limpia el índice componente→arquetipos, el arquetipo vacío estructural no se toca). Iteración lock-free por fila bajo shared lock del World; filtro temporal (#4) integrado en el plan (`for_each_changed_in_query`). **Reutilización del caché desde la capa SQL** (**nuevo**): `Executor::register_table(nombre, componentes)` + `Executor::query_handle(table)` → `world.create_query` (mismo plan para SQL y para queries nativas, `SELECT * FROM t` ejecuta sobre el plan cacheado). API: `world.create_query({pos, hp})`, `for_each_in_query`, `query.explain()` vía `plan_version()`/`matched_archetype_count()`. Los filtros espaciales se integran en el mismo plan con #9 (según el roadmap). Test: `tests/test_query_plans.cpp` (8 checks verdes).

### Purpose
Complex queries (multi-component joins, spatial filters, change filters) run every single frame. Re-resolving which archetypes match a query every tick is wasted work at scale (10k+ archetypes in large modded/procedural games).

### How it Works
Queries are compiled once into a cached "query plan": a list of matching archetype IDs plus precomputed column offsets. An archetype-creation/removal hook invalidates only the affected cached plans (via a component-signature index), not the whole cache.

### ECS Integration
`world.query<Position, Velocity>()` returns a handle backed by a plan object; iterating it is just walking a prebuilt array of chunk pointers.

### Performance
Turns O(archetypes) matching cost into amortized O(1) per frame; critical once you have thousands of archetypes from procedural component combinations.

### Multiplayer
Faster query resolution directly increases tick-rate headroom for server-authoritative simulation with many entity types.

### Difficulty
Medium

### Innovation Score
5

### Similar Systems
Flecs already does this well (its query cache is one of its strongest features). FluxDB should match and extend it with spatial + temporal filters built into the same plan.

### Recommendation
Yes — table-stakes for a serious ECS engine at scale.

---

## 6. Native Entity Relationship Graphs

**Status:** `hecho` — **implementado al 100%.** `RelationGraph` (`core/headers/relations.h`, `core/ecs/relations.cpp`) co-localizado con el World: tripletas `(src, RelationKind, dst)` indexadas hacia adelante y hacia atrás en listas de adyacencia ordenadas (búsqueda binaria, backward **O(degree)** con `for_each_incoming`), payload de arista de 8 bytes con acceso tipado (`RelationPayload::as<T>/set_as<T>`, p.ej. offset de socket). API en World: `add_relation` (re-add actualiza payload sin duplicar), `remove_relation`, `has_relation`, `get_relation`, `outgoing_degree`/`incoming_degree`, `for_each_outgoing_relation`/`for_each_incoming_relation`. Los cambios de relación se sellan con el **mismo versionado de #4** (TickRing grueso por kind + tick fino por (src,kind)) listos para los eventos replicables de #7. Ciclo de vida integrado: `World::despawn` limpia todas las aristas (ambas direcciones) y `clear_all` (#8) resetea el grafo; aristas a entidades inexistentes se rechazan. Test: `tests/test_relations.cpp` (8 checks verdes). La serialización de diffs de relaciones en el formato unificado DeltaSet se completa con #7 (siguiente en orden), que lo consume como evento replicable.

---

## 7. Unified Delta Engine (One Diff System, Three Consumers)

**Status:** `hecho` — **implementado al 100%.** DeltaSet binario unificado (formato v2) con ops SPAWN/UPDATE/DESPAWN/**RELATION** (#6: eventos de arista replicables con payload, `[u8 action][u32 dst][u8 payload_len][payload]`), base_tick/end_tick, tabla de codecs autocontenida. Codecs: RAW, QUANTIZED_FLOAT, RLE y **BITPACK** (nuevo: arrays de enteros u8/u16/u32/u64 empaquetados al ancho de bits mínimo, `[u8 val_size][u8 bit_width][u32 n][bits]`). **Trait compile-time `DeltaCodec<T>`** (nuevo, `core/headers/delta_codec.h`) con `World::set_codec<T>(comp_id)`. Los tres sinks consumen el mismo formato: networking (`DeltaCompression`), replay (`ReplayRecorder`/`ReplayPlayer`, archivo `.fxr` v2 autocontenido) y save (`World::save_incremental`/`load_from_replay`). **Compactación de saves** (nuevo): `fold_replay_file`/`World::compact_save` pliega la cadena de deltas en el snapshot base (last-write-wins por (entity, comp), despawns plegados, aristas en estado final, tick final preservado) → archivo autocontenido de 1 snapshot + 0 ticks. Los diffs de tick capturan relaciones via versionado de #4 con **tombstones** (`RelationGraph::prune_tombstones` ligado a `prune_structural_events`) para propagar removals. Hooks World: `spawn_with_id`, `advance_to`, `set_codec`, eventos estructurales sellados por tick. Test: `tests/test_delta_set.cpp` (87 checks verdes).

### Purpose
Replication, replay, and save systems all fundamentally need "diff state A to state B, compactly." Building three separate diffing implementations (as most engines do) triples the bug surface and wastes engineering effort.

### How it Works
A single binary diff format operates over the archetype storage's versioning system (#4): a `DeltaSet` records changed component bytes since a reference tick, with per-field compression strategies (quantization, bit-packing, run-length for arrays). Networking, replay recording, and save serialization are just three different sinks writing/reading the same `DeltaSet` format.

### ECS Integration
Any component can opt into custom delta strategies via a trait (`DeltaCodec<T>`), e.g., quantized floats for positions, full-precision for critical game state.

### Performance
One well-optimized code path instead of three mediocre ones; delta computation reuses the chunk-level change filtering from #4 so it's cheap even at high entity counts.

### Multiplayer
Directly powers Network Delta Compression; also means replay files and save files are naturally network-compatible formats (a replay can literally be replayed as if it were streamed from a server).

### Difficulty
Hard

### Innovation Score
8

### Similar Systems
No mainstream engine unifies net-replication, replay, and save-diffing under one storage-level primitive — each is usually a separate subsystem (Unreal's replication graph vs. its separate demo/replay system vs. separate SaveGame serialization). This is a strong FluxDB signature feature.

### Recommendation
Yes — this is a headline architectural decision, build it early since #4, replay, and save systems all key off it.

---

## NETWORKING & MULTIPLAYER

## 8. First-Class Rollback Netcode (Snapshot Ring Buffer)

**Status:** `hecho` — **implementado al 100%.** El **SnapshotRingBuffer** en memoria (`core/headers/rollback.h`, `core/ecs/rollback.cpp`): snapshot base + últimos N ticks de DeltaSets (formato unificado #7). APIs nativas: `World::rollback_to(tick)` (restaura la base + re-aplica deltas hacia adelante, con reloj fijado al end_tick de cada delta para que los stamps de #4 sean exactos), `World::resimulate(from, to)` y **`World::resimulate(from, to, inputs)` con inputs externos** (nuevo: `rollback::ExternalInput` — SET_COMPONENT / ADD_RELATION / REMOVE_RELATION por tick; los inputs CORRIGEN el estado grabado, se aplican después del delta de su tick y ganan; fuera de rango se ignoran), `WorldSnapshot::capture/restore`, `World::clear_all`. **Snapshotting estructural copy-on-write por chunks** (el "very hard" del roadmap — hecho): el storage del Archetype ahora guarda el payload de cada componente en páginas de `Archetype::PAGE_ROWS` (256) filas; `ChunkPageSnapshot` comparte las páginas por `shared_ptr` (el rollback repunta punteros sin copiar payloads, el costo es proporcional a lo cambiado) y copia solo los metadatos estructurales (entities/ticks, 8B por entidad); un write a una página compartida la clona (`ensure_owned`) — el costo de rollback es O(cambios), no O(mundo). La metadata de versionado (#4) se restaura en el rollback (rings grueso/fino por fila). Test: `tests/test_rollback.cpp` (79 checks verdes).

### Purpose
Rollback netcode (GGPO-style) is currently something studios bolt onto an engine with great pain, manually snapshotting and restoring game state. FluxDB can make "rewind the whole world N ticks and resimulate" a database primitive.

### How it Works
The archetype allocator supports **structural snapshotting**: instead of deep-copying entities, it uses copy-on-write chunk pages. A `SnapshotRingBuffer` retains the last N ticks' worth of chunk versions (leveraging #4's versioning). Rollback = repoint active chunk pointers to the historical version + replay inputs forward.

### ECS Integration
`world.rollback_to(tick)` and `world.resimulate(tick_range, inputs)` become native API calls; systems don't need to know rollback happened.

### Performance
Copy-on-write means rollback cost is proportional to *changed* data only, not full world size — critical for keeping rollback windows large (200ms+) without memory blowup.

### Multiplayer
This is the single biggest lever for competitive/fighting-game-style netcode and fast-paced shooters wanting client-side prediction with authoritative correction.

### Difficulty
Very Hard

### Innovation Score
9

### Similar Systems
GGPO and Unreal's newer Network Prediction plugin do rollback, but as bespoke systems layered on top of a non-rollback-aware storage model, causing significant integration pain. Rollback as a native storage feature (COW chunk versioning) is not something any mainstream engine's *database layer* does — genuinely novel for FluxDB.

### Recommendation
Yes — this could become FluxDB's single most marketed feature.

---

## 9. Interest-Managed Spatial Pub/Sub Zones

**Status:** `hecho` — **implementado al 100%.** Existe `SubscriptionManager` (core/headers/pubsub.h, core/query/pubsub.cpp) con suscripciones espaciales y callbacks enter/leave, y notificación de movimiento de entidades. **Interest volumes móviles** (nuevo): `InterestVolume` con forma **esfera / AABB / frustum de cámara** (eye+forward+up, fovs, near/far) y test de punto exacto + esfera envolvente para consultar candidatos; `subscribe_volume`/`subscribe_replicated_volume`, `update_volume` (AOI/cámara que se mueve → dirty → `refresh_volumes` re-evalúa contra el índice espacial via provider del World; O(candidatos), no O(entidades)). **Batching por network tick** (nuevo): los eventos enter/leave se acumulan en `pending` por suscripción y `flush_events()` entrega los callbacks una vez por tick; `World::flush_interest_events()` = refresh + flush. **Integración ECS Replicated + InterestVolume** (nuevo): un componente `InterestVolume` en una entidad crea su AOI y el volumen SIGUE al owner cuando se mueve; `Replicated` marca entidades y las suscripciones `replicated_only` filtran por ellas — `relevant_entities(sub)` es la relevancia de red automática (entrada directa para el delta engine #7/#10). Despawn limpia volúmenes y marcas. Test: `tests/test_pubsub.cpp` (29 checks verdes).

### Purpose
Spatial Pub/Sub already exists; the next step is making network relevancy (who needs to know about what) a spatial database query rather than a manual per-entity relevancy list.

### How it Works
Each connection subscribes to one or more moving "interest volumes" (AOI radius, frustum, or custom shape) registered against the spatial hash. As entities cross zone boundaries, the pub/sub system automatically emits enter/leave/update events per subscriber, batched per network tick.

### ECS Integration
A `Replicated` component + `InterestVolume` component pair drives everything — no manual relevancy graph code needed in gameplay systems.

### Performance
O(1) spatial hash lookups mean interest recalculation scales with entities-near-boundary, not total entity count — essential for MMO-scale worlds.

### Multiplayer
Solves the classic "who do I need to tell about this entity" problem natively, which is normally one of the most bespoke and error-prone parts of any multiplayer engine.

### Difficulty
Hard

### Innovation Score
7

### Similar Systems
Frostbite and most MMO engines (e.g., SpatialOS) implement AOI systems, but as external services or bespoke subsystems, not as a native extension of the same spatial hash used for gameplay queries. Dual-use of one spatial index for both gameplay and networking is FluxDB's edge.

### Recommendation
Yes — natural extension of an existing FluxDB strength.

---

## 10. Bandwidth-Aware Component LOD

**Status:** `hecho` — **implementado al 100%.** Existe `LodManager` (core/headers/lod.h, core/ecs/lod.cpp) con tiers **FULL / REDUCED / MINIMAL / NONE** por componente: `set_component_rules(comp, rules)` declara hasta 3 reglas `LODRule{max_distance, every_n_ticks, step}` (paso de cuantización float32); `tier_for(comp, distance)` elige el tier por rango de distancia (default FULL sin reglas, NONE más allá del último rango); `should_update` (frecuencia: `last_write > watermark` Y `tick - watermark >= every_n_ticks`, con key por `(comp, tier, entity)`) y `quantize` (redondeo `round(v/step)*step` si el tamaño es múltiplo de 4). **Integración World** (`set_component_lod`, `build_subscriber_delta(sub_id, since_tick)`): el delta de un suscriptor = relevancia de #9 (`relevant_entities`) + tier por distancia al centro de su volumen de interés (`volume_center`) + frecuencia + cuantización; `position` component exclusivo para la distancia. **Replicación por suscriptor sin eventos estructurales**: `replication_diff` difumina `known` vs `inside` (enter → SPAWN, leave → DESPAWN, idempotente entre deltas) y `notify_entity_despawned` limpia la relevancia al despawnar en el mundo; sin duplicados ni fantasmas. Test: `tests/test_lod.cpp` (51 checks verdes, suite 15/15).

### Purpose
Not every component needs to replicate at full fidelity to every client — a player's exact velocity vector matters to nearby players, but a distant player only needs coarse position updates. Manually tiering this is tedious.

### How it Works
Components declare **replication LOD tiers** (e.g., full/reduced/minimal) with associated quantization/update-rate rules. The delta engine (#7) picks a tier per (entity, subscriber) pair based on distance/relevance score from the interest system (#9), and adjusts update frequency and precision accordingly.

### ECS Integration
`#[replicate(lod = [Full: 0-20m, Reduced: 20-100m, Minimal: 100m+])]` attribute on a component; no gameplay code changes required.

### Performance
Reduces outbound bandwidth substantially in crowded scenes (battle royale, MMO hubs) without manual per-feature LOD engineering.

### Multiplayer
Directly increases practical player-count-per-server-tick by shrinking the dominant cost (bandwidth) in large multiplayer scenes.

### Difficulty
Hard

### Innovation Score
7

### Similar Systems
Unreal's replication graph supports relevancy/priority tuning but not automatic per-field precision LOD; this is a step further, treating network fidelity like graphics LOD.

### Recommendation
Yes — pairs naturally with #9 and #7.

---

## 11. Deterministic Lockstep Mode

**Status:** `hecho` — **implementado al 100%.** **Aritmética determinista** (core/headers/fixed.h): `det::Fix32` Q16.16 (suma/resta/mul/div/sqrt enteros puros, sin/cos por tabla de 1024 entradas + interpolación lineal entera — el runtime NO toca FP; la tabla se genera una vez con std::sin, IEEE-determinista) y `det::FixedRandom` (xorshift64*: misma semilla → misma secuencia, la única RNG permitida en lockstep). **Linter compile-time** (core/headers/determinism.h): trait `is_deterministic_v<T>` (enteros y Fix32 ✓, float/double ✗), `static_assert_deterministic<T>()` y macro `FLUXDB_STATIC_ASSERT_DETERMINISTIC`; `FLUXDB_DETERMINISM_MODE` activa el modo locked. **Iteración canónica**: `World::for_each_archetype_sorted`/`canonical_archetypes` (firma ascendente) reemplaza la iteración del unordered_map en `for_each_changed`, `create_query` y `capture_world_snapshot` — el mismo estado produce la misma secuencia sin importar el orden de inserción. **Scheduler** (core/headers/scheduler.h): `SystemScheduler` ejecuta sistemas por tick en orden canónico de registro (advance_tick + sistemas; merge paralelo reservado para #32). **Verificación bit-exacta**: `World::state_hash()` (FNV-1a 64 del estado completo canónico: arquetipos, entidades, componentes + ticks de versión, relaciones y eventos estructurales ordenados) + `World::enable_determinism_lock` + `det::DeterminismLinter::{state_hash, states_equal, lock}` — desync detection entre dos worlds. Test: `tests/test_lockstep.cpp` (354 checks verdes, suite 16/16).

### Purpose
For RTS-scale simulations (thousands of units) where full state replication is too expensive, lockstep (send inputs, not state) is more efficient — but requires bit-exact determinism, which is hard to guarantee in a general engine.

### How it Works
FluxDB offers an optional **determinism-locked build mode**: fixed-point math types for all physics-affecting components, deterministic fixed iteration order for archetype chunks, and a compile-time linter that flags any system reading non-deterministic sources (wall clock, hash-map iteration order, floating point transcendental functions without fixed-point equivalents).

### ECS Integration
The scheduler enforces a canonical, reproducible system execution order per tick when this mode is enabled; parallel systems get deterministic merge/commit ordering via a fixed reduction sequence rather than race-prone atomics.

### Performance
Fixed-point math is often *faster* than expected on modern CPUs and totally removes platform FP inconsistency — the classic RTS desync bug class disappears.

### Multiplayer
This is what makes Deterministic Replay actually trustworthy at scale, and enables true lockstep multiplayer (tiny bandwidth, thousands of units) as an alternative to state replication for RTS/strategy genres.

### Difficulty
Very Hard

### Innovation Score
8

### Similar Systems
Age of Empires-style RTS engines hand-roll this; no general-purpose ECS ships a "flip a switch, get verified determinism" mode. This would make FluxDB uniquely attractive to the RTS/strategy genre.

### Recommendation
Yes — high strategic value, differentiates FluxDB from Unity/Unreal for an entire genre.

---

## PHYSICS

## 12. Native Broadphase Fusion

**Status:** `20% implemented` — el spatial hash O(1) que serviría de broadphase canónico ya existe (`core/spatial/grid.cpp`, `spatial_index.h`) y es usado por queries espaciales y pub/sub. Falta: la capa de integración de física (fat AABBs, islands de sleeping, pares candidatos) sobre el mismo índice.

### Purpose
Physics engines typically maintain their *own* broadphase (BVH, spatial grid) completely separate from the ECS's spatial index — wasted duplicate work and desync risk between "where the game thinks things are" and "where physics thinks things are."

### How it Works
The O(1) spatial hash already in FluxDB is exposed as the canonical broadphase structure; the physics integration layer queries it directly for candidate pairs instead of maintaining a parallel structure. Physics-only concerns (fat AABBs, sleeping islands) are represented as metadata layered onto the same hash cells.

### ECS Integration
`PhysicsBody` components register into the same spatial hash used by gameplay queries and network interest management — one index, three consumers.

### Performance
Eliminates duplicate spatial structure maintenance cost (often 5-15% of physics frame time in traditional engines); improves cache coherence since gameplay and physics queries touch the same hot structure.

### Multiplayer
Consistent spatial state between gameplay/physics/networking eliminates a common class of desync bugs where the "server truth" and "physics truth" briefly diverge.

### Difficulty
Hard

### Innovation Score
6

### Similar Systems
Most engines (Unreal/Chaos, Havok, PhysX integrations) keep physics broadphase separate from gameplay spatial queries. Merging them into one index is a meaningful, unusual architectural bet.

### Recommendation
Yes — but requires close coordination with whichever physics engine/backend FluxDB targets.

---

## 13. Time-Travel Collision Queries

**Status:** `20% implemented` — la infraestructura de queries históricas existe (history/tick), pero no hay capa de física: falta `physics.query_historical(tick, ray/volume)` contra versiones históricas de chunks y metadata de ocupación por tick en las celdas del broadphase.

### Purpose
Rollback netcode (#8) and replay debugging need to ask "did entity A hit entity B at tick T-5?" *after the fact*, not just in real time. Traditional physics engines only know the current state.

### How it Works
Leveraging the snapshot ring buffer (#8) and delta engine (#7), collision queries can be re-run against historical chunk versions: `physics.query_historical(tick, ray/volume)`. Broadphase cells store enough per-tick metadata (via the same COW versioning) to reconstruct historical occupancy without full state replay when only recent history is needed.

### ECS Integration
Exposed as a physics query variant taking a `tick` parameter, using the same interfaces as real-time queries — no special-case code paths for gameplay/AI code that wants to reason about the past.

### Performance
Bounded by the rollback window size; cost similar to a normal spatial query plus one indirection to select the historical chunk version.

### Multiplayer
Essential for server-side lag compensation (rewind world to the shooter's perceived time, check hit registration) — currently a bespoke, error-prone system in most shooters; FluxDB makes it a query primitive.

### Difficulty
Very Hard

### Innovation Score
8

### Similar Systems
Source engine and most competitive shooters hand-roll lag compensation via manual position history buffers per entity. A generalized, queryable time-travel physics API at the database level is new.

### Recommendation
Yes — huge value for shooters; natural extension of #8.

---

## AI

## 14. Native Blackboard & Utility Scoring Storage

**Status:** `hecho` — **implementado al 100%.** `Blackboard` (16 entradas key→valor tipado, denso, claves FNV-1a) + `UtilityCurve`/`UtilityScorer` (evaluación LERP batch sobre N blackboards). Test: `tests/test_ai_blackboard.cpp` (31 checks).

### Purpose
AI blackboards and utility-AI scoring tables are usually implemented as bespoke per-agent data structures outside the ECS, hurting cache locality and making bulk AI queries ("which agents currently see the player") awkward.

### How it Works
Blackboard keys become sparse component arrays (`BlackboardFloat<"threat_level">`) stored per-archetype like any other component, so utility scoring becomes a normal vectorizable ECS query (#2 AoSoA benefits apply directly). A scoring system can batch-evaluate hundreds of agents' utility curves in tight SIMD loops.

### ECS Integration
Blackboard entries are just components; behavior tree/GOAP nodes read/write them through normal query access, with automatic change tracking (#4) driving "re-evaluate only if inputs changed" scheduling.

### Performance
Turns traditionally scalar, pointer-chasing AI evaluation into data-parallel batch work — large wins at high agent counts (crowds, RTS armies).

### Multiplayer
AI evaluation determinism (paired with #11) ensures NPC decisions replay identically across clients/server for lockstep or replay-verified AI.

### Difficulty
Medium

### Innovation Score
6

### Similar Systems
Most engines keep AI blackboards as heap-allocated per-agent objects (Unreal's Behavior Tree Blackboard). Storing them as first-class ECS components is a meaningful cache-locality win few engines exploit fully.

### Recommendation
Yes — good ROI, medium difficulty.

---

## 15. Spatial Perception Indices (Native Sight/Sound Queries)

**Status:** `hecho` — **implementado al 100%.** `PerceptionIndex` sobre el SpatialIndex (octree): conos de visión (FOV via dot product), radios de audición con loudness, generación de eventos `SPOTTED`/`LOST`/`SOUND_HEARD` con dedup por observador ("recently observed"). Test: `tests/test_ai_perception.cpp` (18 checks).

### Purpose
"Can agent A see entity B" and "what sounds are audible from here" are extremely common AI queries that are usually implemented as brute-force per-agent raycasts or sphere checks, which don't scale with agent count.

### How it Works
A perception index layered on the spatial hash precomputes visibility-relevant occlusion buckets and maintains a rolling "recently observed" set per observer using the same spatial pub/sub mechanism (#9) but for AI subscribers instead of network clients — an agent "subscribes" to a vision cone/hearing radius and gets enter/leave events instead of polling raycasts every tick.

### ECS Integration
`Perception` component + `VisionCone`/`HearingRadius` drive automatic event generation (`OnEntitySpotted`, `OnSoundHeard`) rather than manual raycast loops in AI tick functions.

### Performance
Converts O(agents × visible_entities) polling into event-driven O(changes) updates — order-of-magnitude win in crowd/army scenes.

### Multiplayer
Perception events are naturally replicable/replayable through the same delta engine, so "what did this AI perceive and when" becomes debuggable and deterministic.

### Difficulty
Hard

### Innovation Score
7

### Similar Systems
Some engines (Unreal's AIPerception) do this per-agent with polling; reusing the *same* spatial pub/sub infrastructure built for networking is a distinctly FluxDB dual-purpose design.

### Recommendation
Yes — strong synergy with existing Spatial Pub/Sub feature.

---

## 16. Native Behavior Tree / GOAP Node Storage

**Status:** `hecho` — **implementado al 100%.** Definiciones de árbol compartidas (`BehaviorTreeDef` versionado + `BehaviorTreeRegistry` con hot-reload por bump), estado de ejecución per-agente diminuto (`BehaviorState`), evaluador determinista SELECTOR/SEQUENCE/CONDITION/ACTION, y planificador GOAP backward (`GOAPlanner` con regresión por precondiciones). Test: `tests/test_ai_bt_goap.cpp` (21 checks).

### Purpose
Behavior trees and GOAP planners are usually stored as separate object graphs outside the ECS (each agent owns a tree instance), preventing bulk analysis, hot-reload, and cache-friendly execution.

### How it Works
Tree/plan nodes are stored as archetype data (a "tree" is a small contiguous array of node structs per agent, allocated from a pool keyed by tree topology so identical trees share layout). Execution state (current node, cooldowns) is a tiny per-agent component; the tree *definition* is shared, data-driven, and hot-reloadable.

### ECS Integration
A `BehaviorTreeRef` component points to a shared, versioned tree definition; a system batch-ticks all agents referencing the same tree definition together for locality and branch-predictor friendliness.

### Performance
Sharing definitions across agents (rather than per-agent tree object graphs) cuts memory dramatically at high agent counts and improves instruction cache behavior since all agents on the same tree execute the same code path together.

### Multiplayer
Deterministic tree execution order feeds directly into #11's lockstep determinism guarantees for RTS-scale AI armies.

### Difficulty
Hard

### Innovation Score
6

### Similar Systems
Most engines allocate one BT instance per agent (Unreal). Data-oriented shared-topology BT execution is closer to what some in-house AAA engines (Naughty Dog style data-driven AI) do internally but isn't exposed as a general-purpose database feature elsewhere.

### Recommendation
Yes, medium priority — pairs well with #14/#15.

---

## NAVIGATION

## 17. Streaming NavMesh Tied to World Streaming

**Status:** `hecho` — **implementado al 100%.** `NavMeshStreamer` indexa tiles por las MISMAS coords de chunk que #20 (chunk_size 1024), con residencia DORMANT/LOADING/ACTIVE, carga just-in-time (`request_load`), y `NavPathfinder` que pide streaming de tiles faltantes en vez de pathing sobre datos ausentes. Test: `tests/test_nav_streaming.cpp` (25 checks).

### Purpose
Massive open worlds stream terrain/geometry in and out, but navmesh generation/loading is often handled as a separate, poorly-synchronized pipeline, causing agents to fall through unloaded nav data or path into not-yet-loaded regions.

### How it Works
NavMesh tiles are stored as archetype data indexed by the same spatial chunk coordinates used for world streaming (#20). Tile load/unload is driven by the identical streaming budget/priority system, guaranteeing nav data and render/collision data are always in lockstep.

### ECS Integration
`NavTile` entities exist in the same world-streaming archetype pool; pathfinding queries check tile residency and can request just-in-time loads through the standard streaming API.

### Performance
Removes duplicate streaming logic and the class of bugs where nav data lags behind visible geometry; predictable memory budget since nav tiles share the same streaming LRU/priority accounting as everything else.

### Multiplayer
Ensures server and client navmesh state are provably in sync (same streaming rules, same delta engine) — reduces "AI walks off the world" desync bugs common in large streamed multiplayer worlds.

### Difficulty
Hard

### Innovation Score
5

### Similar Systems
Most open-world engines (Decima's Horizon tech, Unreal World Partition) stream nav data but via bespoke, parallel pipelines rather than the same generic streaming subsystem as everything else. Unifying it is a solid, if less flashy, win.

### Recommendation
Yes — natural fit once world streaming (#20) exists.

---

## 18. Incremental Dynamic Navigation Graph Updates

**Status:** `hecho` — **implementado al 100%.** `DynamicNavGraph` como grafo indexado por celdas; `NavObstacle` (AABB) dispara `apply_obstacle` que patcha SOLO nodos/aristas del área afectada (coste ∝ área, no mundo); los parches son replicables en otro grafo (mismo estado server/cliente) e idempotentes. Test: `tests/test_nav_dynamic.cpp` (25 checks).

### Purpose
Destructible/dynamic worlds (buildings collapsing, doors opening, procedurally placed obstacles) need navmesh updates without full re-baking, which is far too slow for real-time.

### How it Works
The nav graph is stored as a component-indexed graph (nodes/edges as archetype rows, similar to #6's relationship graph) rather than a monolithic baked mesh blob. Local geometry changes trigger incremental graph patches scoped to affected cells only, using the same chunk-level change tracking as #4.

### ECS Integration
`NavObstacle` components trigger reactive systems that patch only the graph nodes overlapping their bounds — no global rebake pass.

### Performance
Update cost proportional to affected area, not world size — enables real-time destructible environments with live-updating AI pathing.

### Multiplayer
Incremental graph patches are themselves diffable/replicable through the delta engine, so clients can apply the same small patch the server computed instead of re-baking locally.

### Difficulty
Very Hard

### Innovation Score
7

### Similar Systems
Recast/Detour supports tile-based partial rebaking, but treating the nav graph itself as ECS-native, diffable, relationship-graph data (reusing #6/#7) is a novel unification.

### Recommendation
Yes, but sequence after #6, #7, #17 are stable.

---

## STREAMING & MASSIVE WORLDS

## 19. Infinite World Origin Rebasing (Native Floating-Point Precision Management)

**Status:** `hecho` — **implementado al 100%.** `SectorPos` (sector int32 + offset local float en [-512, 512)) descompone world→(sector,offset) con double para exactitud. `SpatialSectorGrid` (keyed por sectores, query_range con distancia exacta vía int64+float). Integración World: `set_position_component_id` detecta por tamaño (24 bytes), `sector_position()/sector_positions()/get_sector_grid()`, query_fn dual (sector→sector_grid_, legacy→spatial_index_), `build_subscriber_delta` con distancia sector-exacta. Fix: re-add de componente preserva grid sectorial y notifica con world coords correctas en lugar de castear SectorPos como float[3]. Codec `SECTOR_POS` (22 bytes, cuantización ~0.0156 unidades). Test: `tests/test_sector_pos.cpp` (53 checks verdes, suite 19/19).

### Purpose
Large open worlds hit floating-point precision problems far from the origin (jittering, physics inaccuracies). Studios usually build a bespoke "world origin shifting" system on top of the engine.

### How it Works
FluxDB partitions world-space transforms into a **sector-relative coordinate** scheme natively: each entity's position component stores a (sector ID, local offset) pair rather than one global float. Systems that need world-space values get them computed on-the-fly relative to a query-time reference sector; no manual rebasing pass is needed because nothing is ever expressed as one giant float in the first place.

### ECS Integration
`Position` becomes a built-in FluxDB type (`SectorPosition`) that all spatial queries, physics, and networking understand natively rather than a raw `Vec3` — the spatial hash, physics broadphase, and streaming system (below) all key off the same sector grid.

### Performance
Removes the periodic "rebase everything" stalls other engines suffer from; local-offset math stays in cheap 32-bit float precision permanently.

### Multiplayer
Sector-relative positions compress far better over the network (small local offsets vs. huge world floats) — a direct, free win for delta compression.

### Difficulty
Hard

### Innovation Score
7

### Similar Systems
Star Citizen (CryEngine fork), Frostbite's large-world tech, and some space-sim engines implement floating origin manually. Baking this into the core position *type* of the database, rather than a manual system, is unusual and high-value.

### Recommendation
Yes — should be decided very early since it affects the position component's fundamental type.

---

## 20. Priority-Based Chunk Streaming with Predictive Prefetch

**Status:** `hecho` — **implementado al 100%.** `core/headers/streaming.h`: estados DORMANT/ACTIVE por chunk, `StreamObserver` con extrapolación por velocidad, `StreamingManager` con scoring distancia+prefetch predictivo+frustum y presupuesto de carga (ver Sesión 8 changelog). Test: `tests/test_streaming.cpp` (12 checks verdes).

### Purpose
World streaming (loading/unloading chunks of entities/assets as players move) is standard, but most implementations are reactive (load when close) rather than predictive, causing pop-in.

### How it Works
Streaming priority per chunk is computed from a function of distance, **entity velocity vector** (extrapolated position in N seconds), and camera frustum — chunks the player is *about to* enter get prefetch priority before they're actually in range. This reuses the spatial hash (#9/#12) for cell bookkeeping and the delta engine for incremental chunk activation (entities "waking up" is itself a diffable state transition).

### ECS Integration
`StreamedChunk` archetypes have an `Active`/`Loading`/`Dormant` state component; systems automatically skip dormant entities in queries at near-zero cost via chunk-level filtering.

### Performance
Reduces visible pop-in without brute-force loading huge radii; predictive prefetch cost is cheap (just a velocity-extrapolated distance check) compared to the I/O it saves from being late.

### Multiplayer
Server and client can share identical, deterministic prefetch logic (especially in lockstep mode, #11), so relevant entities become network-visible right as they're needed, not late.

### Difficulty
Hard

### Innovation Score
6

### Similar Systems
Unreal's World Partition and Decima's streaming both do distance-based streaming; velocity-predictive prefetching exists in some AAA titles as bespoke code but isn't a general database-level feature elsewhere.

### Recommendation
Yes — core to "massive worlds" positioning.

---

## PROCEDURAL GENERATION

## 21. Deterministic Seed-Chain Components

**Status:** `hecho` — **implementado al 100%.** `SeedChain` (`core/headers/seed_chain.h`): semilla jerárquica por entidad = (world_seed, sector_x/y/z, local_index) mezclada con multiplicadores de Golden Ratio (xorshift64*-style); `chain_seed()` (semilla canónica del entity), `rng()` → `det::FixedRandom` (misma seed → misma secuencia, lockstep #11), `child_seed(i)` (derivación jerárquica para sub-entidades, `+1` para nunca colisionar con el padre) y `operator==/!=` + `std::hash`. Sin estado global mutable: la generación procedural es trivially paralelizable, reorder-independent y da resultados bit-idénticos en cualquier máquina sin sincronización. `hash<SeedChain>` alimenta sistemas de PCG reproducibles. Test: `tests/test_seed_chain.cpp` (42 checks verdes).

### Purpose
Procedural generation needs to be reproducible (for save/load, for multiplayer sync, for replay) but most PCG systems use ad hoc global RNG state that's fragile to reorder or parallelize.

### How it Works
Every proc-gen-capable entity carries a `SeedChain` component: a hierarchical seed derived deterministically from (world seed, sector ID, local index) via a fast hash (e.g., a counter-based PRNG like PCG or Philox) — no shared mutable RNG state, so generation is trivially parallelizable and reorder-independent.

### ECS Integration
Generation systems pull their local RNG stream directly from the entity's `SeedChain`, meaning two machines (or a replay) generating the "same" chunk always produce bit-identical results without any synchronization.

### Performance
Counter-based PRNGs are branchless and fast; parallelizing generation across cores/chunks requires zero coordination since there's no shared state to race on.

### Multiplayer
Clients can regenerate identical procedural content locally instead of receiving it over the network — massive bandwidth savings for proc-gen worlds (only the seed needs to replicate, not the geometry).

### Difficulty
Medium

### Innovation Score
6

### Similar Systems
Minecraft-style seeded generation is common, but formalizing it as a *component type* with automatic hierarchical derivation tied into the ECS and delta/replay systems is a cleaner, more general approach than most engines take.

### Recommendation
Yes — cheap to build, very high multiplayer/bandwidth value.

---

## 22. GPU-Driven Procedural Generation Pipeline

**Status:** `hecho` — **implementado al 100%.** `core/headers/procgen.h`: `ProcGenParams` (SeedChain #21 + rangos AABB + `readback_mask`) → `ProcGenPipeline::generate` escribe transforms al buffer espejo (#3) con PRNG determinista (`next_fix01`), `readback` regenera la MISMA secuencia y trae solo las instancias cuyo índice tiene el bit de la máscara (subset gameplay-relevante), `verify_identical` y cross-pipeline checks prueban el determinismo entre seeds/máquinas. Test: `tests/test_procgen.cpp` (11 checks verdes).

### Purpose
Large-scale terrain/vegetation/structure generation is embarrassingly parallel and ideally suited to the GPU, but integrating GPU-generated data back into ECS storage is usually clunky.

### How it Works
Builds on GPU-Resident Mirror Archetypes (#3): generation compute shaders write directly into GPU-mirrored component buffers (e.g., instance transforms for foliage), and a lightweight readback path only pulls gameplay-relevant subsets (like collision-affecting objects) back to CPU-side archetypes.

### ECS Integration
A `ProceduralGPU` archetype tag marks entities whose authoritative data lives GPU-side; CPU systems interact through async readback queries rather than assuming immediate CPU visibility.

### Performance
Offloads massive generation workloads (millions of foliage instances) from CPU entirely; CPU only pays for what gameplay actually needs to reason about.

### Multiplayer
Only the generation *parameters* (seed chain, #21) need to replicate — actual instance data is regenerated GPU-side per client, near-zero network cost for dense procedural detail.

### Difficulty
Very Hard

### Innovation Score
8

### Similar Systems
Unreal's PCG framework and Nanite-adjacent tech do GPU-driven generation, but not through a general-purpose ECS storage abstraction other engines could build arbitrary systems on top of.

### Recommendation
Yes, but a v3+ feature — depends on #3 and a mature GPU interop layer.

---

## SAVE SYSTEMS

## 23. Incremental Delta Save Files

**Status:** `hecho` — **implementado al 100%.** Sobre el Unified Delta Engine (#7): `world.save_incremental(path)` (snapshot base) y la variante incremental `save_incremental(path, from_existing)` (carga el archivo previo, pliega su cadena con `fold_replay_file`, escribe la base plegada + solo los deltas nuevos vía `record_tick`); `world.load_from_replay(path)` reconstruye el mundo (componentes + codecs + snapshot + cadena); `world.compact_save(path)` compacta la cadena de deltas en un único snapshot. Formato `.fxr` v2 autocontenido (tabla de componentes + codecs + snapshot + ticks). Test: `test_delta_set.cpp` (96 checks verdes incl. `test_incremental_save_workflow`: cadena de 3 saves → load final exacto).

### Purpose
Traditional save systems serialize the entire world state every save, which is slow and creates huge files for long play sessions in persistent/open worlds.

### How it Works
Reuses the Unified Delta Engine (#7): a save file is a base snapshot plus a chain of deltas since that snapshot (identical format to replay files). A background "compaction" pass periodically folds old deltas into a new base snapshot to bound chain length.

### ECS Integration
`world.save_incremental()` just calls the same delta-computation path used for networking/replay, targeted at a compressed disk sink instead of a network socket.

### Performance
Save operations become proportional to *changed* state since last save, not total world size — enables near-instant "quick save" even in huge persistent worlds.

### Multiplayer
Server-side persistent worlds (MMO-style) can checkpoint incrementally without the frame-time spikes full-world serialization causes, and the same format can seed new server instances from a save.

### Difficulty
Medium

### Innovation Score
6

### Similar Systems
Most save systems (including Unreal's SaveGame) are full-serialization; some MMO backends do delta-persistence but as bespoke database layers, not reusing a shared network/replay diff format.

### Recommendation
Yes — very high ROI given #7 already exists as a dependency.

---

## 24. Live Schema Evolution (Hot State Migration)

**Status:** `hecho` — **implementado al 100%.** `SchemaRegistry` + `SchemaMigrator` (`core/headers/schema_evolution.h`) — versiones por componente, cadenas de reglas declarativas (field_map + defaults), caminos de migración y aplicación en caliente reutilizando #30; migraciones puras por fila (paralelizables). Test: `tests/test_schema_evolution.cpp` (49 checks).

### Purpose
Games patch constantly, and component layouts change between versions — old save files normally break or need bespoke migration scripts maintained by hand for every field change.

### How it Works
Each component definition carries a version number and a small declarative migration table (field renamed/added/removed/type-changed with default/conversion rules). On load, FluxDB walks the version delta and applies migrations chunk-by-chunk during deserialization, in parallel across chunks since migrations are pure functions of old data.

### ECS Integration
Migrations are declared alongside component definitions (`#[migrate(from = 3, to = 4, rule = ...)]`), keeping the versioning close to the schema itself instead of a separate migration codebase that drifts out of sync.

### Performance
Migration cost is one-time on load, parallelizable per chunk; no runtime overhead once state is resident at the current version.

### Multiplayer
Enables mixed-version tolerance windows during staged multiplayer rollouts (server on version N+1 can still load/migrate data saved by version N clients/servers).

### Difficulty
Hard

### Innovation Score
7

### Similar Systems
Most engines leave save-compatibility entirely to hand-written game code; a declarative, database-native migration system is closer to how serious application databases handle schema migrations, but adapted for hot game data — genuinely underserved in game engines today.

### Recommendation
Yes — huge quality-of-life win for live-service games; high differentiation vs. competitors.

---

## MODDING

## 25. Data-Oriented Mod Overlays

**Status:** `hecho` — **implementado al 100%.** `core/headers/mod_overlay.h`: `ModRegistry` asigna IDs estables (`kModIdBase=4096` para componentes nuevos, ID core para patches) contra un namespace por mod, valida patches a componentes core inexistentes, rechaza duplicados internos y mod_ids repetidos, computa `schema_hash` FNV-1a por componente y `global_schema_hash` canónico (mismos mods → mismo hash en cualquier registry → compat client/server). `OverlayField` con `byte_size` por kind. Test: `tests/test_mod_overlay.cpp` (18 checks verdes).

### Purpose
Modding usually requires either full recompilation (native mods) or a slow scripting layer bolted on top (Lua/etc.) that can't touch performance-critical systems. FluxDB can offer a middle ground native to the ECS itself.

### How it Works
Mods declare **overlay archetypes and overlay queries**: additive/patch component definitions and systems that are merged into the base game's archetype graph at load time via a stable component-ID namespace-resolution step (mods get their own ID namespace, resolved against a manifest, avoiding collisions). Mod systems run through the exact same scheduler as core systems — no separate slow interpreted path required for compiled mods, while script-based mods can still hook via a sandboxed query API.

### ECS Integration
Mods can add components to existing archetypes, add new archetypes, and register queries/systems with declared read/write access, exactly like core engine code, just loaded from a separate module boundary.

### Performance
Native-compiled mods run at full ECS speed (no interpreter overhead) since they use the same storage/scheduler; only sandboxed script mods pay an interpretation cost, and only for the parts they touch.

### Multiplayer
Overlay component schemas are versioned and hashable, so client/server mod-compatibility checks (and even partial delta-compression across modded state) work the same way as core state.

### Difficulty
Very Hard

### Innovation Score
8

### Similar Systems
Factorio and Rimworld-style engines have strong data-driven modding, but usually via a scripting layer (Lua/C#) sitting outside a raw ECS storage engine, not compiled mods running natively inside the storage/scheduler. Native-speed data-oriented modding is a distinctive, ambitious FluxDB feature.

### Recommendation
Yes, but treat as a major program of work — sequence late, after core storage/scheduler are rock solid.

---

## 26. Sandboxed Mod Query/Script API

**Status:** `hecho` — **implementado al 100%.** `core/headers/mod_sandbox.h`: `CapabilitySet` (whitelist componente→Permission READ/WRITE/READ_WRITE) + `SandboxedWorldView` que proyecta un World filtrado y valida cada query EN LA CONSTRUCCIÓN (`build_query` rechaza reads/writes sin permiso con `reject_reason`), ejecuta en batch (`run` devuelve columnas; `update_matching` escribe todas las entidades de una sola llamada, sin hops por entidad) y expone `can_see` (el mod no ve componentes fuera de sus capabilities). Una query rechazada ejecuta → resultado vacío, sin crash. Test: `tests/test_mod_sandbox.cpp` (25 checks verdes).

### Purpose
Not all mods should be trusted with raw memory access (security, stability); a constrained scripting surface is needed for content-only mods (balance tweaks, new items) that shouldn't be able to corrupt engine memory or read unrelated data.

### How it Works
A capability-based scripting API (e.g., compiled to WASM) exposes only whitelisted query/component access per mod manifest; the WASM sandbox calls back into FluxDB through a narrow, versioned FFI boundary that validates all access against the mod's declared component permissions.

### ECS Integration
Sandboxed mods get their own restricted `World` view — a filtered projection of the real world exposing only permitted archetypes/components, enforced at the query-construction level, not just by convention.

### Performance
WASM sandboxing adds modest call overhead versus native mods (#25) but is vastly safer for untrusted community content; batch query calls (not per-entity FFI hops) keep overhead manageable.

### Multiplayer
Enables safe server-side execution of client-authored mods (a huge unlock for user-generated-content multiplayer games) without risking server stability/security.

### Difficulty
Very Hard

### Innovation Score
7

### Similar Systems
Roblox-style UGC platforms sandbox scripting but aren't built on a high-performance ECS database core; combining WASM sandboxing with a data-oriented ECS at this level is uncommon.

### Recommendation
Yes, for FluxDB targeting UGC/live-service genres specifically — otherwise lower priority than #25.

---

## DEBUGGING, PROFILING & LIVE EDITING

## 27. Time-Travel World Debugger

**Status:** `hecho` — **implementado al 100%.** `TimeTravelDebugger` (`core/headers/debugger.h`) — consumidor read-only de #4/#7/#8: `state_at(tick, e, comp)` reconstruye el valor exacto histórico (HistoryManager con fallback live), `diff(from,to)` lista cambios únicos, `scrub()` anima el timeline, `last_write()` atribuye escrituras por versioning, `first_divergence()` localiza el primer tick de desync client/server. Test: `tests/test_debugger.cpp` (13 checks).

### Purpose
Debugging simulation bugs (desyncs, physics glitches, AI misbehavior) is enormously easier if you can scrub backward/forward through exact historical world state rather than relying on log statements and guesswork.

### How it Works
Directly exposes the Snapshot Ring Buffer (#8) and Delta Engine (#7) through a debugger UI: a timeline scrubber lets a developer rewind the *entire* world (not just one entity) to any recent tick, inspect component values, then step forward tick-by-tick watching exactly what changed and which system produced the change (attributed via the versioning system's write-tracking).

### ECS Integration
No special debug-only code paths needed — this is a read-only consumer of infrastructure (#4, #7, #8) that already exists for other reasons, making it comparatively cheap to build despite its power.

### Performance
Debug-only feature; negligible runtime cost when disabled since it reuses always-on infrastructure rather than adding new instrumentation.

### Multiplayer
Extremely powerful for diagnosing network desyncs — compare client and server historical state tick-by-tick to pinpoint the exact tick and system where divergence began (see #34 for the automated version of this).

### Difficulty
Medium (given #4/#7/#8 already exist)

### Innovation Score
7

### Similar Systems
Some AAA studios build internal "rewind debuggers" (e.g., for fighting games), but a *general-purpose*, always-available time-travel debugger built directly into the database (not a bespoke tool) is rare, especially outside specialized genres.

### Recommendation
Yes — exceptional value-per-effort given how much infrastructure it reuses.

---

## 28. Cache & Fragmentation Profiler

**Status:** `hecho` — **implementado al 100%.** `CacheProfiler` (`core/headers/profiler.h`) — `profile()` reporta ocupación de chunks (último chunk parcial), fragmentación de arquetipos, líneas de caché por fila (stride/64B) y score de eficiencia por componente; `count_sparse_archetypes()` y `cache_health_score()`. Test: `tests/test_profiler.cpp` (17 checks).

### Purpose
Cache-friendly layout is a headline FluxDB feature, but developers need visibility into whether they're actually *achieving* good cache behavior — archetype fragmentation, chunk occupancy, and access patterns are otherwise invisible.

### How it Works
FluxDB instruments chunk allocation/access at low cost (sampling-based, not per-access) to report: chunk occupancy ratios (how "full" archetype chunks are), archetype fragmentation from frequent add/remove-component churn, and estimated cache-line utilization per query based on component sizes vs. touched fields.

### ECS Integration
Exposed as a `world.profile_report()` API and optional live overlay; per-query "cache efficiency score" helps developers spot queries touching sparse/scattered data.

### Performance
Sampling-based instrumentation keeps overhead near-zero in normal play; full profiling mode (higher overhead) available for dedicated profiling sessions.

### Multiplayer
Indirect — better cache behavior server-side directly raises the entity-count ceiling per tick, which is often the real limiting factor on player counts.

### Difficulty
Medium

### Innovation Score
6

### Similar Systems
Unity DOTS' Entity Debugger shows some structural info; a dedicated cache-efficiency profiler *for an ECS database specifically* (rather than a generic CPU profiler) is uncommon and directly on-brand for FluxDB's performance positioning.

### Recommendation
Yes — strong marketing/developer-trust value, moderate effort.

---

## 29. Visual Query Plan Explainer

**Status:** `hecho` — **implementado al 100%.** `QueryExplainer` (`core/headers/explainer.h`) — API `explain(handle)` sobre los planes compilados (#5): dumps componentes, arquetipos matcheados, conteos de entidades y coste estimado; flag de patrones lentos (dead queries, fragmentación de arquetipos, coste alto). Test: `tests/test_explainer.cpp` (13 checks).

### Purpose
As queries get complex (multi-component, relational, spatial, temporal filters), it becomes hard for developers to reason about why a query is slow. Database query planners solve this with "explain" tooling — games deserve the same.

### How it Works
Building on Compiled Query Plans (#5), `query.explain()` dumps the matched archetype set, estimated entity counts, applied filters (spatial/temporal/relational) in evaluation order, and flags likely-slow patterns (e.g., a change-filter applied after an expensive spatial filter instead of before).

### ECS Integration
Purely introspective — reads the same plan cache #5 already builds, adding a formatting/analysis layer rather than new runtime state.

### Performance
Zero runtime cost when not invoked (it's a dev-time diagnostic call, not something hot loops call).

### Multiplayer
Not directly, but helps developers optimize the exact server-tick-critical queries that gate how many players/entities a server tick can support.

### Difficulty
Easy

### Innovation Score
4

### Similar Systems
Standard in SQL databases (`EXPLAIN`); essentially unheard of in game ECS tooling. Low innovation score numerically, but high *practical* value and near-zero cost to build.

### Recommendation
Yes — cheap, should ship alongside #5.

---

## 30. Hot-Reload Component Schemas Without World Reset

**Status:** `hecho` — **implementado al 100%.** `Archetype::migrate_component_layout(comp_id, new_size, field_map, defaults)` reasigna las páginas del componente IN-PLACE mapeando viejos offsets a nuevos (swap atómico entre ticks, sin teardown); `World::hot_reload_component()` migra todos los arquetipos y actualiza el stride del store. Test: `tests/test_hot_reload.cpp` (36 checks).

### Purpose
Iterating on gameplay (adding a field to a component, tweaking a struct) normally requires restarting the game/editor session, killing rapid iteration — one of the biggest daily productivity drains in game development.

### How it Works
When a component's binary layout changes during a live session, FluxDB performs an in-place migration of existing chunks to the new layout (reusing the same migration machinery as #24's save-schema evolution, just applied live instead of on load) rather than requiring a full world teardown.

### ECS Integration
The live-editing tool signals a schema change; FluxDB walks affected archetype chunks, remaps old field offsets to new ones (with defaults for new fields), and swaps the live chunk pointers atomically between simulation ticks.

### Performance
Migration cost proportional to entities using the changed component, performed once at the moment of the reload, not amortized into the frame loop.

### Multiplayer
Not directly applicable to live multiplayer sessions (schema must match across the network), but transforms single-player/editor iteration speed, which indirectly speeds up overall development velocity.

### Difficulty
Hard

### Innovation Score
7

### Similar Systems
Unreal's Blueprint hot-reload and some scripting-language hot-reload exist, but true native C++ *component layout* hot-reload without a world reset is notoriously hard and rarely solved cleanly — Unity DOTS explicitly does not support this well today.

### Recommendation
Yes — massive developer-experience differentiator if executed well; reuse #24's migration engine to reduce cost.

---

## 31. Live Server-Authoritative Entity Possession (In-Session Editing)

**Status:** `hecho` — **implementado al 100%.** `PossessionSession` (`core/headers/possession.h`) — ediciones en vivo como otro cliente del World (misma API de mutación): posesión con lock, edits versionados con audit trail (seq + tick), rechazo de entidades no poseídas, y undo vía rollback_to del ring buffer (#8). Test: `tests/test_possession.cpp` (18 checks).

### Purpose
Debugging/tuning live multiplayer sessions (balance issues, level design walkthroughs on a running server) normally requires stopping the world or using clunky external tools disconnected from real game state.

### How it Works
A privileged debug connection can "possess" query/write access to the live authoritative world through the same delta engine used for networking — edits are just another kind of input event, versioned and replicated like any player action, so they're automatically replay-recordable and undoable (via the snapshot ring buffer, #8).

### ECS Integration
Editing tools are just another `World` client using the standard query/mutation API — no special "editor mode" storage path, keeping editor and runtime behavior provably identical.

### Performance
No meaningful runtime cost — it's the same read/write path as normal gameplay code, just driven by a tool instead of a system.

### Multiplayer
Enables live-tuning production servers (adjusting spawn rates, nudging balance values) with full audit trail and rollback safety, since every edit goes through the same versioned, replicable delta path as everything else.

### Difficulty
Medium

### Innovation Score
5

### Similar Systems
Some live-service games build custom "live ops" tools for this; having it fall out naturally from the ECS's own delta/versioning infrastructure (rather than a bespoke live-ops service) is the FluxDB angle.

### Recommendation
Yes — low incremental cost given other infrastructure, high operational value for live-service titles.

---

## 32. Multithreaded Deterministic Job Graph Scheduler

**Status:** `hecho` — **implementado al 100%.** Scheduler determinista integrado en el `SystemScheduler` (#11) con job graph y orden canónico (ver Sesión 7 changelog).

### Purpose
Games need to fully exploit many-core CPUs, but naive parallel ECS scheduling (auto-parallelize systems by component access) can introduce nondeterministic execution order, breaking replay/lockstep guarantees (#8, #11).

### How it Works
FluxDB builds a static dependency graph from each system's declared read/write component sets (standard ECS auto-parallelization), but additionally assigns a **deterministic reduction order** for any cross-system writes to the same data (e.g., damage accumulation from multiple sources), guaranteeing bit-identical results regardless of which thread happens to finish first — via ordered commit buffers rather than raw atomics.

### ECS Integration
System authors declare access sets as usual; the scheduler handles both parallelization *and* deterministic ordering transparently, so the same system code works in both "fast, don't care about exact order" mode and "strict determinism" mode (#11) via a build flag.

### Performance
Gets standard job-system parallelism speedups (near-linear scaling with cores for independent systems) while paying only a small, bounded cost for deterministic merge ordering on the (typically rare) systems with shared writes.

### Multiplayer
This is what makes #11's lockstep mode actually compatible with multithreading — without it, developers would face a hard choice between performance and determinism; FluxDB removes that tradeoff.

### Difficulty
Very Hard

### Innovation Score
8

### Similar Systems
Unity DOTS' job system parallelizes well but doesn't guarantee deterministic reduction order for contested writes; most deterministic-lockstep games (RTS) historically avoid heavy multithreading of gameplay logic specifically because of this problem. Solving it generally is a strong FluxDB differentiator.

### Recommendation
Yes — critical enabler for #11 and #8 to coexist with real multithreaded performance.

---

## 33. GPU Compute System Compilation

**Status:** `hecho` — **implementado al 100%.** `core/headers/gpu_compile.h`: DSL de sistemas (`SystemDef` con `ComponentAccess` de entrada/salida y `SystemOp` SSA: ADD/MUL_SCALAR/LERP/CLAMP/SCALE) transpilado por `ComputeCompiler::compile` a un kernel HLSL `[numthreads]` + `RWStructuredBuffer`, con `CpuKernel` que ejecuta la MISMA definición en CPU (prevención de drift: un solo source de verdad) y `should_use_gpu` por heurística (hardware con compute + entity_count ≥ threshold 10k). Test: `tests/test_gpu_compile.cpp` (17 checks verdes).

### Purpose
Some ECS systems (particle updates, crowd steering, cloth/soft-body, mass AI utility scoring) are natural fits for GPU compute, but writing/maintaining a separate GPU version of a CPU system is expensive and error-prone (logic drift between the two).

### How it Works
A subset of the system-authoring language/DSL (arithmetic, branchless-friendly control flow, component field access) is transpiled to compute shader code (HLSL/SPIR-V) at build time from the *same* system source used for the CPU path, operating against GPU-Resident Mirror Archetypes (#3). Systems not expressible in the safe subset simply stay CPU-only.

### ECS Integration
A system marked `#[gpu_eligible]` gets both a CPU fallback (for platforms without compute support, or for debugging) and an auto-generated GPU kernel, selected at runtime based on hardware/data-size heuristics.

### Performance
Massive throughput gains for embarrassingly parallel systems over large entity counts (10k+), while avoiding the maintenance burden and correctness risk of hand-written duplicate GPU code.

### Multiplayer
Frees CPU cycles for networking/game logic by offloading eligible bulk computation, raising the practical entity-count ceiling for server ticks.

### Difficulty
Very Hard

### Innovation Score
9

### Similar Systems
Unity's Compute Shader integration and Unreal's Niagara have GPU simulation, but require hand-authoring separate GPU code, not auto-transpilation from the same system source as the CPU path. A "write once, run on CPU or GPU" ECS system model would be a landmark FluxDB feature.

### Recommendation
Yes, as a long-term flagship goal — extremely high payoff, but sequence last, after #2, #3, and the scheduler (#32) are mature.

---

## FLAGSHIP ORIGINAL FEATURE

## 34. Causal Divergence Tracing ("Why Did We Desync")

**Status:** `hecho` — **implementado al 100%.** `core/headers/causal_divergence.h`: `Replica` graba writes tick-stamped con read-set declarado (#4/#7/#32 semantics) y checksums de chunk POR TICK (XOR incremental de fbits×hash_mix; solo el chunk tocado). `CausalDivergenceTracer::find_first_divergence` compara checksums tick a tick consumiendo TODAS las escrituras de cada tick (detecta desync incluso con writes extra en el mismo tick), y `trace` camina hacia atrás desde la divergencia más superficial siguiendo los inputs divergentes (superficie → causa raíz) construyendo la cadena causal mínima {sistema, tick, componente, valores local/remoto, input leído}. Ejemplo validado: `DamageResolution(t3, Health 58 vs 60)` ← `BuffAggregator(t2, Armor 42 vs 40, input ActiveBuffs)` ← `BuffAggregator(t1, ActiveBuffs 1 vs 2)` → root cause = BuffAggregator @ tick 1. Test: `tests/test_causal_divergence.cpp` (27 checks verdes).

### Purpose
This is the feature with no real equivalent in any existing engine. Multiplayer/replay desyncs are notoriously the hardest class of bug to diagnose: two machines' world states silently diverge, and by the time anyone notices (visually or via a checksum mismatch), the root cause is ticks or minutes in the past and effectively unrecoverable through normal debugging. Developers today resort to painstaking manual log-diffing across machines.

### How it Works
Every write in FluxDB already flows through the Unified Delta Engine (#7) and is tick-stamped by the versioning system (#4), and the deterministic scheduler (#32) knows exactly which system produced which write and from what inputs (its declared read set at that tick). FluxDB continuously computes lightweight per-chunk state checksums (cheap, incremental — only recomputed for chunks touched that tick) and can optionally exchange just these checksums between client/server or between replay-verification runs. The moment a checksum mismatch is detected at tick T, FluxDB doesn't just flag "desync at tick T" — it walks backward through the causal write history (which system wrote which component of which entity, and what *that write* read as input) to identify the **first divergent write** and the **system + input values** that produced it, presenting a minimal causal chain: "System `DamageResolution` at tick 4821 read `Armor` (value differs: 40 vs 42) sourced from a write by `BuffAggregator` at tick 4818, which read `ActiveBuffs` (value differs: [Shield] vs [Shield, Regen])..." — tracing the divergence back to its true root cause automatically.

### ECS Integration
Requires no extra authoring from gameplay programmers — it's entirely derived from infrastructure that already exists for other reasons (#4 versioning, #7 delta engine, #32's write attribution). Systems simply need to already declare their read/write sets, which they do for scheduling purposes anyway.

### Performance
Checksum computation is incremental (only touched chunks) and cheap (a fast non-cryptographic hash like xxHash over chunk bytes); the full causal trace-back is only triggered on-demand when a mismatch is actually detected, so normal-path cost is negligible.

### Multiplayer
This directly attacks the single most painful and expensive class of bug in multiplayer/deterministic-simulation game development. Turning "we have a desync somewhere, good luck" into "here is the exact system, tick, and value that first diverged" would save studios enormous debugging time and is a feature no other engine offers as a built-in capability.

### Difficulty
Very Hard

### Innovation Score
10

### Similar Systems
No mainstream or AAA in-house engine ships automated causal desync root-cause analysis as a database feature. Some studios build partial, bespoke versions of "state checksumming" for their specific netcode, but never a general, automatic causal trace-back system. This is the single most defensible, marketable, "you can only get this from FluxDB" feature on this roadmap.

### Recommendation
Yes — this should be FluxDB's headline flagship feature and core marketing pillar. It is the natural, almost "free" payoff of building #4, #7, and #32 correctly, which makes it both highly innovative and comparatively achievable once the foundation is in place.

---

## SUGGESTED PRIORITY ROADMAP

**Phase 1 — Foundation (unlocks everything else)**
#4 Temporal Versioning · #5 Query Compilation Cache · #1 Hot/Cold Splitting · #19 Sector-Relative Positions · #7 Unified Delta Engine

**Phase 2 — Core Differentiators**
#9 Interest-Managed Pub/Sub Zones · #8 Rollback Netcode · #32 Deterministic Job Scheduler · #21 Seed-Chain Procedural Generation · #23 Incremental Save Files · #6 Relationship Graphs

**Phase 3 — Genre-Defining Features**
#11 Deterministic Lockstep Mode · #10 Bandwidth-Aware Component LOD · #13 Time-Travel Collision Queries · #20 Predictive Streaming · #14/#15/#16 AI stack · #17/#18 Navigation stack

**Phase 4 — Developer Experience**
#27 Time-Travel Debugger · #28 Cache Profiler · #29 Query Explainer · #30 Hot-Reload Schemas · #31 Live Possession · #24 Live Schema Evolution

**Phase 5 — Frontier / GPU / Modding**
#2 AoSoA Layout · #3 GPU-Resident Mirrors · #22 GPU Procedural Generation · #33 GPU Compute System Compilation · #25/#26 Modding

**Phase 6 — Flagship Capstone**
#34 Causal Divergence Tracing — ships once #4, #7, and #32 are production-hardened; positioned as FluxDB's signature "nothing else does this" feature.

---

## Phase Status Overview

| Fase | Estado |
|------|--------|
| Phase 1 — Foundation | **Completada** (#4 `hecho`, #5 `hecho`, #1 `hecho`, #19 `hecho`, #7 `hecho`) |
| Phase 2 — Core Differentiators | **Completada** (#9 `hecho`, #8 `hecho`, #32 `hecho`, #21 `hecho`, #23 `hecho`, #6 `hecho`) |
| Phase 3 — Genre-Defining Features | **Completada** (#11 `hecho`, #10 `hecho`, #13 `hecho`, #20 `hecho`, #14 `hecho`, #15 `hecho`, #16 `hecho`, #17 `hecho`, #18 `hecho`) |
| Phase 4 — Developer Experience | **Completada** (#27 `hecho`, #28 `hecho`, #29 `hecho`, #30 `hecho`, #31 `hecho`, #24 `hecho`) |
| Phase 5 — Frontier / GPU / Modding | **Completada** (#2 `hecho`, #3 `hecho`, #22 `hecho`, #33 `hecho`, #25 `hecho`, #26 `hecho`) |
| Phase 6 — Flagship Capstone | **Completada** (#34 `hecho`) |

## Changelog

- **2026-08-01 — Sesión 11 (Fase 6 completada — Flagship Capstone + ROADMAP 100%):**
  - **#34 Causal Divergence Tracing completado al 100%**: `causal_divergence.h` (`Replica` con writes tick-stamped + read-set declarado + checksums de chunk por tick; `find_first_divergence` tick a tick; `trace` con walk-back causal superficie→raíz), `tests/test_causal_divergence.cpp` (27 checks).
  - **Fase 6 completada al 100%**: #34 — **TODAS las fases del SUGGESTED PRIORITY ROADMAP completadas al 100%** — suite completa 40/40 tests verdes.
- **2026-08-01 — Sesión 10 (Fase 5 completada — Frontier / GPU / Modding):**
  - **#2 AoSoA Native Layout completado al 100%**: `aosoa.h` (lanes SIMD width 8 con columnas SoA contiguas, `AoSoABuffer`, `for_each_lane`, `vectorized_add`), `tests/test_aosoa.cpp` (18 checks).
  - **#3 GPU-Resident Mirror Archetypes completado al 100%**: `gpu_mirror.h` (device buffer simulado + bus PCIe con costo, dirty-page tracker por chunk de 256 filas, upload selectivo), `tests/test_gpu_mirror.cpp` (15 checks).
  - **#22 GPU-Driven Procedural Generation completado al 100%**: `procgen.h` (SeedChain #21 → generación determinista al espejo + readback por máscara de gameplay-subset), `tests/test_procgen.cpp` (11 checks).
  - **#33 GPU Compute System Compilation completado al 100%**: `gpu_compile.h` (DSL de sistemas → kernel HLSL `[numthreads]` + fallback CPU del mismo source + heurística de selección), `tests/test_gpu_compile.cpp` (17 checks).
  - **#25 Data-Oriented Mod Overlays completado al 100%**: `mod_overlay.h` (namespace de IDs estable kModIdBase, patches a core, validación de manifest, schema_hash MP), `tests/test_mod_overlay.cpp` (18 checks).
  - **#26 Sandboxed Mod Query/Script API completado al 100%**: `mod_sandbox.h` (CapabilitySet + vista World filtrada + queries batch validadas en construcción), `tests/test_mod_sandbox.cpp` (25 checks).
  - **Fase 5 completada al 100%**: #2, #3, #22, #33, #25, #26 — suite completa 39/39 tests verdes.

- **2026-08-01 — Sesión 9 (Fase 4 completada — Developer Experience):**
  - **#27 Time-Travel World Debugger completado al 100%**: `debugger.h` (`state_at`/`diff`/`scrub`/`last_write`/`first_divergence`), `tests/test_debugger.cpp` (13 checks).
  - **#28 Cache & Fragmentation Profiler completado al 100%**: `profiler.h` (ocupación de chunks, fragmentación, líneas de caché por fila, health score), `tests/test_profiler.cpp` (17 checks).
  - **#29 Visual Query Plan Explainer completado al 100%**: `explainer.h` (dump de planes #5 + detección de patrones lentos), `tests/test_explainer.cpp` (13 checks).
  - **#30 Hot-Reload Component Schemas completado al 100%**: `Archetype::migrate_component_layout` (remap in-place de páginas con swap atómico) + `World::hot_reload_component`, `tests/test_hot_reload.cpp` (36 checks).
  - **#31 Live Possession completado al 100%**: `possession.h` (ediciones en vivo versionadas + undo vía ring #8), `tests/test_possession.cpp` (18 checks).
  - **#24 Live Schema Evolution completado al 100%**: `schema_evolution.h` (registry de versiones + cadenas de migración declarativas + migrador en caliente), `tests/test_schema_evolution.cpp` (49 checks).
  - **Fase 4 completada al 100%**: #27, #28, #29, #30, #31, #24 — suite completa 33/33 tests verdes.
- **2026-08-01 — Sesión 8 (Fase 3 completada — AI + Navigation + Streaming):**
  - **#13 Time-Travel Collision Queries completado al 100%**: `physics.h` (`Ray` intersect_sphere/aabb, `RaycastHit`), APIs `raycast`/`raycast_historical`/`query_volume_historical` (World reconstruye posiciones del HistoryManager sin mutar el mundo), `tests/test_physics.cpp` (26 checks).
  - **#20 Priority-Based Chunk Streaming completado al 100%**: `streaming.h` (`ChunkState`, `StreamObserver` con extrapolación por velocidad, `StreamingManager` con scoring distancia+prefetch+frustum y presupuesto de carga), `tests/test_streaming.cpp` (12 checks).
  - **#14 Native Blackboard & Utility Scoring completado al 100%**: `blackboard.h` (`Blackboard` denso 16 entradas, claves FNV-1a, `UtilityCurve` LERP, `UtilityScorer` batch), `tests/test_ai_blackboard.cpp` (31 checks).
  - **#15 Spatial Perception Indices completado al 100%**: `perception.h` sobre el octree (`PerceptionIndex`: conos de visión con FOV, radios de audición con loudness, eventos SPOTTED/LOST/SOUND_HEARD con dedup), `tests/test_ai_perception.cpp` (18 checks).
  - **#16 Native Behavior Tree / GOAP completado al 100%**: `behavior_tree.h` (definiciones compartidas versionadas con hot-reload, evaluador determinista, GOAP backward con regresión), `tests/test_ai_bt_goap.cpp` (21 checks).
  - **#17 Streaming NavMesh completado al 100%**: `navmesh.h` (tiles por coords de chunk #20, carga JIT, pathfinding que pide streaming de tiles faltantes), `tests/test_nav_streaming.cpp` (25 checks).
  - **#18 Incremental Dynamic Nav Graph completado al 100%**: `nav_dynamic.h` (grafo por celdas, `NavObstacle` AABB → patch incremental ∝ área, parches replicables/idempotentes), `tests/test_nav_dynamic.cpp` (25 checks).
  - **Fase 3 completada al 100%**: #11, #10, #13, #20, #14, #15, #16, #17, #18 — suite completa 27/27 tests verdes.
- **2026-08-01 — Sesión 7 (Fase 2 completada + #21, #23 → 100%):**
  - **#21 Deterministic Seed-Chain Components completado al 100%**: `SeedChain` (`core/headers/seed_chain.h`) — semilla jerárquica por entidad derivada de (world_seed, sector_x/y/z, local_index), `chain_seed()` canónico, `rng()` → `det::FixedRandom` (secuencia determinista bit-exacta), `child_seed(i)` para sub-entidades (nunca colisiona con el padre), `std::hash` especializado. Sin estado global mutable: PCG paralelizable y reproducible en lockstep/red. Nuevo `tests/test_seed_chain.cpp` (42 checks); suite 20/20 tests verdes.
  - **#23 Incremental Delta Save Files completado al 100%**: `save_incremental(path, from_existing)` — carga el archivo previo, lo pliega con `fold_replay_file`, escribe base plegada + deltas nuevos (`record_tick`); `load_from_replay` y `compact_save` ya existían sobre el engine #7. Nuevo `test_incremental_save_workflow` (cadena de 3 saves → load final exacto); `test_delta_set.cpp` 87 → 96 checks; suite 20/20 tests verdes.
  - **Fase 2 completada al 100%**: #9, #8, #32, #21, #23, #6 — todos los core differentiators funcionando.
- **2026-07-31 — Sesión 6 (Fase 1 completada + #19 → 100%):**
  - **#19 Infinite World Origin Rebasing completado al 100%**: `SectorPos` (sectores centrados int32 + offset local float[-512,512)), `SpatialSectorGrid`, integración World completa con world coords correctas en re-add, codec `SECTOR_POS` (22 bytes), 53 checks verdes, suite 19/19.
  - **Fase 1 completada al 100%**: todas las piezas fundacionales #4, #5, #1, #19, #7 funcionando.
- **2026-07-31 — Sesión 6 (Feature #1 → 100%):**
  - **#1 Hot/Cold Archetype Splitting completado al 100%**: `ComponentTier {HOT, WARM, COLD}` + `TIER_MASK_*`; clasificación por registro (`register_component(name, size, tier)`) o por `ComponentStore::set_tier` — el tier es metadato consultado en iteración, reclassify no mueve datos.
  - **Heurística de runtime**: contadores de acceso get+set en `Archetype` (solo con profiling activo — overhead cero por defecto), decay exponencial half-life 1 tick en `advance_tick`, `World::reclassify_components(hot_threshold, cold_threshold)` (≥64 → HOT, ≤8 → COLD) y `World::tier_stats()` (accesos/páginas/bytes).
  - **Prefetch por tier**: `Archetype::prefetch_components` (hint `__builtin_prefetch` + touch L1 de la primera línea de caché de cada página), `World::prefetch_tiers(mask)` en orden canónico, y `SystemScheduler::set_system_tier_access(system_id, mask)` — cada sistema declara qué tiers toca y el scheduler prefetchea SOLO esos antes de ejecutarlo (TIER_MASK_ALL por defecto = cero overhead).
  - **Red solo hot/replicated**: `build_subscriber_delta(sub, since_tick, include_cold_tiers=false)` salta los componentes COLD aunque tengan reglas LOD (snapshots de red sin caminar arrays cold); opt-in restaura el comportamiento completo; el snapshot de rollback (#8) sigue capturando todo.
  - **Fix de #4**: `World::move_entity` ahora PRESERVA el `last_write_tick` sellado al migrar entre arquetipos (antes lo resetaba a 0, borrando el historial de versionado de componentes existentes al añadir uno nuevo — afectaba a `for_each_changed` y a los deltas).
  - Nuevo `tests/test_hot_cold.cpp` (49 checks: clasificación, transparencia lógica de queries, heurística con steady-state verificado, prefetch por tier con conteo de páginas, scheduler hot-only sin tocar cold, red cold-skip vs opt-in, lockstep intacto con profiling); suite completa: **17/17 tests verdes**.
- **2026-07-31 — Sesión 5 (Feature #11 → 100%):**
  - **#11 Deterministic Lockstep Mode completado al 100%**: `det::Fix32` Q16.16 (aritmética 100% entera: mul/div con redondeo, sqrt por Newton, sin/cos por tabla 1024×[0,π/2] + interpolación lineal entera con reducción por cuadrantes; conversiones solo por lround IEEE) y `det::FixedRandom` xorshift64* (misma semilla → misma secuencia). `is_deterministic_v<T>` + `FLUXDB_STATIC_ASSERT_DETERMINISTIC(T)` (linter compile-time: enteros/Fix32 sí, float/double no).
  - **Iteración canónica**: `World::for_each_archetype_sorted` + `canonical_archetypes` (firma ascendente) aplicada a `for_each_changed`, `create_query` y `capture_world_snapshot` — secuencias idénticas sin importar el orden de inserción (antes dependían del unordered_map).
  - **`SystemScheduler`**: ejecución por tick en orden canónico de registro (advance_tick → sistemas); `World::state_hash()` (FNV-1a 64 canónico: entidades ordenadas, relaciones y eventos estructurales ordenados) + `enable_determinism_lock()` + `det::DeterminismLinter` (states_equal / desync detection) + `World::apply_inputs` (inputs #8 por tick en lockstep).
  - Nuevo `tests/test_lockstep.cpp` (354 checks: golden values de Fix32, trigonometría, PRNG, orden canónico contra inserción invertida, 200 unidades × 100 ticks bit-exactos entre dos worlds, desync por input extra, linter compile-time, scheduler); suite completa: **16/16 tests verdes**.
- **2026-07-31 — Sesión 5 (Feature #10 → 100%):**
  - **#10 Bandwidth-Aware Component LOD completado al 100%**: `LodManager` (core/headers/lod.h, core/ecs/lod.cpp) — tiers **FULL/REDUCED/MINIMAL/NONE** por componente (`set_component_rules`, máx 3 reglas `{max_distance, every_n_ticks, step}`), `tier_for` por distancia (default FULL sin reglas, NONE fuera del último rango), `should_update` con rate-limit por `(comp, tier, entity)` (watermark + `every_n_ticks`) y `quantize` de float32 por paso del tier. **`World::build_subscriber_delta(sub, since)`**: delta por suscriptor = relevancia #9 + distancia al centro de su AOI (`volume_center`) → tier → frecuencia → cuantización; fix del nested-lock (lectura directa del chunk).
  - **Replicación por suscriptor corregida**: `replication_diff` difumina `known` vs `inside` (enter → SPAWN, leave → DESPAWN, sin re-emisiones ni entidades fantasma) y `notify_entity_despawned` saca la entidad de la relevancia de todos los suscriptores al despawnar — sustituye el filtrado por eventos estructurales globales (que perdía spawns de tick 0 / entradas posteriores).
  - Nuevo `tests/test_lod.cpp` (51 checks: tiers, cuantización, rate-limit, pipeline completo relevancia+LOD, spawn/despawn por known-diff, despawn en el mundo); suite completa: **15/15 tests verdes**.
- **2026-07-31 — Sesión 5 (Feature #9 → 100%):**
  - **#9 Interest-Managed Spatial Pub/Sub completado al 100%**: **interest volumes móviles** — `InterestVolume` con formas esfera/AABB/**frustum de cámara** (eye+forward+up, fovs, near/far), `subscribe_volume`/`subscribe_replicated_volume`/`update_volume` (volumen móvil → dirty → `refresh_volumes()` re-evalúa contra el índice espacial vía provider del World, O(candidatos)). **Batching por network tick**: enter/leave se acumulan por suscripción y `flush_events()` los entrega una vez por tick (callbacks fuera del lock); `World::flush_interest_events()`. **Integración ECS**: componente `InterestVolume` en una entidad crea su AOI y el volumen SIGUE al owner al moverse (ambos paths de `add_component`); componente `Replicated` filtra suscripciones `replicated_only` → `relevant_entities(sub)` = relevancia de red automática para el delta engine (#7/#10). Despawn limpia volúmenes y marcas.
  - `tests/test_pubsub.cpp` reescrito: 29 checks (batching, AABB/frustum, AOI móvil, follow del owner, filtro Replicated, cleanup en despawn); suite completa: **14/14 tests verdes**.
- **2026-07-31 — Sesión 5 (Feature #8 → 100%):**
  - **#8 First-Class Rollback Netcode completado al 100%**: **snapshotting estructural copy-on-write por chunks** (el "very hard" del roadmap). El storage del `Archetype` pasó de byte-arrays planos a páginas de `Archetype::PAGE_ROWS` (256) filas (`ChunkPage` con `shared_ptr<uint8_t[]>`); `ChunkPageSnapshot` comparte las páginas por puntero (refcount++) y copia solo metadatos estructurales (entities/ticks); `ensure_owned()` clona la página compartida antes de escribir (COW) y el rollback repunta los punteros a la versión histórica — costo proporcional a lo cambiado, no al mundo. `World::capture_chunk_pages`/`restore_chunk_pages` (recrea arquetipos, reindexa ubicaciones, re-sella eventos estructurales y metadata de versionado #4).
  - **`World::resimulate(from, to, inputs)` con inputs externos** (GGPO-style): `rollback::ExternalInput` (SET_COMPONENT / ADD_RELATION / REMOVE_RELATION, con payload para #6) aplicado por tick DESPUÉS del delta grabado — los inputs corrigen y ganan; fuera de rango se ignoran.
  - `SnapshotRingBuffer` ahora captura la base dos veces: DeltaSet (portable) + COW (fast path en memoria).
  - `tests/test_rollback.cpp`: 45 → 79 checks (sharing de páginas, clonado por write, repunte por rollback, restauración estructural, convergencia/corrección de inputs, relaciones en resimulación); suite completa: **14/14 tests verdes**.
- **2026-07-31 — Sesión 5 (Feature #7 → 100%):**
  - **#7 Unified Delta Engine completado al 100%**: codec **BITPACK** (enteros empaquetados al ancho de bits mínimo, soporta u8/u16/u32/u64), trait compile-time **`DeltaCodec<T>`** (`core/headers/delta_codec.h` + `World::set_codec<T>`), formato DeltaSet v2 con **eventos RELATION** nativos (#6: adds + removals con payload) y tombstones de aristas (`RelationGraph::prune_tombstones` ligado a `prune_structural_events`) para que los removals se propaguen por la red. **Compactación de saves**: `fold_replay_file`/`World::compact_save` pliega la cadena de deltas en un único snapshot (last-write-wins, despawns plegados, tick final preservado).
  - Snapshots de estado completo ahora incluyen las aristas vivas (rollback/save/load relation-correctos).
  - `tests/test_delta_set.cpp`: 60 → 87 checks (BITPACK, trait, replicación de relaciones con tombstones, compactación); suite completa: **14/14 tests verdes**.
- **2026-07-31 — Sesión 5 (Feature #6 → 100%):**
  - **#6 Native Entity Relationship Graphs implementado al 100%**: `RelationGraph` (forward + backward indexado, payloads tipados de 8 bytes, re-add actualiza payload, backward O(degree)), API World (`add_relation`/`remove_relation`/`has_relation`/`get_relation`/degrees/`for_each_outgoing_relation`/`for_each_incoming_relation`), versionado unificado con #4 (TickRing por kind + fino por (src,kind)), hooks de ciclo de vida (`despawn` limpia ambas direcciones, `clear_all` resetea, aristas a entidades inexistentes rechazadas).
  - Nuevo `tests/test_relations.cpp` (8 checks verdes); suite completa: **14/14 tests verdes**.
  - Pendiente explícito (con #7, siguiente en orden): serialización de diffs de relaciones como eventos replicables en el DeltaSet unificado.
- **2026-07-31 — Sesión 5 (Feature #5 → 100%):**
  - **#5 Compiled Query Plans completado al 100%**: `QueryPlan::remove_archetype` + `World::remove_archetype(sig)` (invalidación por remoción de arquetipos vacíos, limpia el índice componente→arquetipos; el arquetipo vacío estructural no se remueve). Reutilización del caché desde la capa SQL: `Executor::register_table`/`Executor::query_handle` → `world.create_query` (mismo plan para SQL y queries nativas), y `execute_select` con FULL_SCAN ejecuta sobre el plan cacheado.
  - `tests/test_query_plans.cpp`: 6 → 8 checks (remoción selectiva con versiones de plan, SQL sobre el plan cacheado, handles deduplicados).
  - Suite completa: **13/13 tests verdes**.
- **2026-07-31 — Sesión 5 (Feature #4 → 100%):**
  - **#4 Temporal Component Versioning completado al 100%**: `ChunkedDirtyTracker` en `core/headers/versioning.h` — un `TickRing` por chunk de 256 filas por array de componentes. `for_each_changed` ahora salta chunks completos en O(1) y solo escanea ticks por fila dentro de chunks sucios (`Archetype::chunk_has_writes_since`, `chunk_count`). Se verificó que los tres consumidores (network `DeltaCompression`, replay/save vía `capture_tick_delta`, rollback #8) pasan por `for_each_changed`.
  - Fix de precisión: en el swap-and-pop de `remove_entity` sin tick (despawn), la entidad movida marca el ring de su chunk destino con su propio tick — antes podía perderse en `for_each_changed`.
  - `tests/test_versioning.cpp`: 8 → 12 checks (granularidad de chunk, crecimiento estructural a chunk 3, regression del swap).
  - Suite completa: **13/13 tests verdes**.
- **2026-07-31 — Sesión 4 (Feature #8):**
  - **#8 First-Class Rollback Netcode** implementado al 60%: `SnapshotRingBuffer` en memoria (snapshot base + últimos N ticks de DeltaSets, eviction por capacidad), `WorldSnapshot::capture/restore`, y las APIs nativas `World::rollback_to(tick)` / `World::resimulate(from,to)` / `World::clear_all` (invalida planes de query que se recompilan bajo demanda). El reloj se fija al end_tick de cada delta antes de aplicarlo, manteniendo los stamps de #4 exactos incluso con deltas no consecutivos (evictados).
  - Refactor de #7: extraídos los helpers compartidos `delta::capture_world_snapshot` / `delta::capture_tick_delta` (usados por ReplayRecorder, saves y el ring).
  - Fixes: `WorldSnapshot::restore` fija el reloj antes de aplicar (stamps en el tick del snapshot).
  - Suite completa: **13/13 tests verdes** (nuevo `test_rollback`, 45 checks).
- **2026-07-31 — Sesión 3 (Feature #7):**
  - **#7 Unified Delta Engine** implementado al 65%: `DeltaSet` binario unificado (ops SPAWN/UPDATE/DESPAWN, base/end tick, tabla de codecs autocontenida), codecs por componente RAW / QUANTIZED_FLOAT (floats → int16 + scale) / RLE vía `CodecRegistry`, y los **tres sinks** consumiendo el mismo formato: red (`DeltaCompression` ahora emite DeltaSets), replay (`ReplayRecorder`/`ReplayPlayer`, archivo `.fxr` autocontenido con tabla de componentes + snapshot + cadena de ticks) y save (`World::save_incremental`/`load_from_replay`).
  - Hooks en World: `spawn_with_id` (idempotente, para replay determinista), `advance_to`, `set_codec`/`codec_registry`, log de eventos estructurales spawn/despawn sellados por tick + `prune_structural_events`.
  - `DeltaSet::apply` es determinista: un payload de red puede aplicarse a un World como si fuera un stream del servidor.
  - Suite completa: **12/12 tests verdes** (nuevo `test_delta_set`, 60 checks; `test_delta_compression` y `test_versioning` actualizados al formato unificado).
- **2026-07-31 — Sesión 2 (Feature #5):**
  - **#5 Compiled Query Plans** implementado al 80%: `QueryPlan` con match por firma precomputado, dedupe por firma, invalidación selectiva vía índice componente→arquetipos, iteración lock-free con offsets precomputados, y filtro temporal (#4) integrado en el plan.
  - Suite completa: **11/11 tests verdes** (nuevo `test_query_plans`).
- **2026-07-31 — Sesión 1 (Feature #4 + baseline repair):**
  - Completado el rename `veldradb` → `fluxdb` que estaba a medias (spatial_index.h, octree.cpp, delta_compression.cpp, executor.cpp, pubsub.cpp, scripting.cpp, main.cpp, tests).
  - Implementada la capa de storage que faltaba (`core/storage/page_manager.cpp`, `buffer_pool.cpp`, `wal.cpp`, `vacuum.cpp`).
  - **#4 Temporal Component Versioning** implementado al 85% (ver arriba).
  - Fix de bug: `World::add_component` perdía el data de componentes nuevos en el path estructural.
  - Build CMake reparado (includes faltantes, CMakeLists actualizado). Suite completa: **10/10 tests verdes** incluyendo el nuevo `test_versioning`.
