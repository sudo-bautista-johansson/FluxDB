#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <shared_mutex>
#include <cmath>

namespace fluxdb {
namespace query {

// ── Interest volumes (#9) ─────────────────────────────────────
// Un volumen de interés (AOI esférico, AABB o frustum de cámara) define
// qué porción del mundo le importa a un suscriptor. Los volúmenes pueden
// MOVERSE (cámara/player): update_volume() los marca dirty y
// refresh_volumes() los re-evalúa contra el índice espacial.
enum class VolumeShape : uint8_t {
    SPHERE = 0, // centro + radio (AOI clásico)
    AABB = 1,   // caja alineada a ejes
    FRUSTUM = 2 // pirámide truncada (cámara): eye + forward + up + fovs + near/far
};

struct InterestVolume {
    VolumeShape shape = VolumeShape::SPHERE;

    // SPHERE: centro | AABB: min | FRUSTUM: eye
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    float r = 10.0f; // SPHERE

    float max_x = 0.0f, max_y = 0.0f, max_z = 0.0f; // AABB: max

    float fwd_x = 0.0f, fwd_y = 0.0f, fwd_z = -1.0f; // FRUSTUM: forward (normalizado)
    float up_x = 0.0f, up_y = 1.0f, up_z = 0.0f;     // FRUSTUM: up de referencia
    float near_p = 0.1f, far_p = 1000.0f;            // FRUSTUM: planos
    float hfov = 1.0472f, vfov = 0.7854f;            // FRUSTUM: ~60°x45° en radianes

    // Test de punto exacto por forma.
    bool contains(float x, float y, float z) const {
        switch (shape) {
        case VolumeShape::SPHERE: {
            float dx = x - cx, dy = y - cy, dz = z - cz;
            return dx * dx + dy * dy + dz * dz <= r * r;
        }
        case VolumeShape::AABB:
            return x >= cx && x <= max_x && y >= cy && y <= max_y && z >= cz && z <= max_z;
        case VolumeShape::FRUSTUM: {
            float vx = x - cx, vy = y - cy, vz = z - cz;
            float depth = vx * fwd_x + vy * fwd_y + vz * fwd_z;
            if (depth < near_p || depth > far_p) return false;
            float rx = fwd_y * up_z - fwd_z * up_y;
            float ry = fwd_z * up_x - fwd_x * up_z;
            float rz = fwd_x * up_y - fwd_y * up_x;
            float rl = std::sqrt(rx * rx + ry * ry + rz * rz) + 1e-9f;
            rx /= rl; ry /= rl; rz /= rl;
            float ux = ry * fwd_z - rz * fwd_y;
            float uy = rz * fwd_x - rx * fwd_z;
            float uz = rx * fwd_y - ry * fwd_x;
            float lateral = vx * rx + vy * ry + vz * rz;
            float vertical = vx * ux + vy * uy + vz * uz;
            float th = std::tan(hfov * 0.5f) * depth;
            float tv = std::tan(vfov * 0.5f) * depth;
            return std::fabs(lateral) <= th && std::fabs(vertical) <= tv;
        }
        }
        return false;
    }

    // Esfera envolvente del volumen (para consultar candidatos al índice
    // espacial). El test exacto lo hace contains().
    void bounding_sphere(float& bx, float& by, float& bz, float& br) const {
        switch (shape) {
        case VolumeShape::SPHERE:
            bx = cx; by = cy; bz = cz; br = r;
            break;
        case VolumeShape::AABB:
            bx = (cx + max_x) * 0.5f;
            by = (cy + max_y) * 0.5f;
            bz = (cz + max_z) * 0.5f;
            br = 0.5f * std::sqrt((max_x - cx) * (max_x - cx) +
                                  (max_y - cy) * (max_y - cy) +
                                  (max_z - cz) * (max_z - cz));
            break;
        case VolumeShape::FRUSTUM: {
            float th = std::tan(hfov * 0.5f), tv = std::tan(vfov * 0.5f);
            float m = th > tv ? th : tv;
            float half_depth = (far_p - near_p) * 0.5f;
            bx = cx + fwd_x * (near_p + half_depth);
            by = cy + fwd_y * (near_p + half_depth);
            bz = cz + fwd_z * (near_p + half_depth);
            br = far_p * m + half_depth;
            break;
        }
        }
    }
};

// Evento de relevancia batch (por network tick).
struct InterestEvent {
    uint32_t entity;
    bool entered; // true = entró al volumen, false = salió
};

// Suscripción espacial nativa.
struct QuerySubscription {
    uint32_t sub_id;
    std::string target_prefab; // vacío = cualquier entidad
    InterestVolume volume;     // (#9) volumen de interés (móvil)
    bool replicated_only = false; // (#9) solo entidades con Replicated

    std::function<void(uint32_t entity_id, bool entered)> callback;

    // (#9) Relevancia actual (conjunto dentro del volumen) + batch pendiente.
    std::unordered_set<uint32_t> inside;
    std::vector<InterestEvent> pending;
    bool volume_dirty = false; // volumen móvil: refresh pendiente

    // (#10) Entidades que el suscriptor YA conoce (spawn entregado). El
    // delta por suscriptor (build_subscriber_delta) difumina relevance vs
    // known: enter → SPAWN, leave → DESPAWN, sin eventos estructurales.
    std::unordered_set<uint32_t> known;
};

class SubscriptionManager {
public:
    SubscriptionManager() = default;

    // Candidato con posición (el índice espacial devuelve ids; el World
    // resuelve las posiciones vía su store).
    struct SpatialCandidate {
        uint32_t entity;
        float x, y, z;
    };

    // (#9) Provider registrado por el World: consulta candidatos en la
    // esfera envolvente del volumen (SpatialIndex::query_range + posiciones).
    using EntityQueryFn = std::function<void(float cx, float cy, float cz, float r,
                                             std::vector<SpatialCandidate>& out)>;
    void set_entity_query_fn(EntityQueryFn fn) { query_fn_ = std::move(fn); }

    // (#9) Filtro de entidades replicables (para suscripciones replicated_only).
    using ReplicatedCheckFn = std::function<bool(uint32_t entity)>;
    void set_replicated_check_fn(ReplicatedCheckFn fn) { replicated_check_fn_ = std::move(fn); }

    // Register a new spatial listener (radio clásico = volumen esférico).
    uint32_t subscribe_spatial(const std::string& prefab, float x, float y, float z, float r,
                               std::function<void(uint32_t, bool)> cb);

    // (#9) Suscripción con volumen de interés arbitrario (esfera/AABB/frustum).
    // El backfill (entidades ya dentro) se resuelve en el próximo
    // refresh_volumes() y se entrega en el siguiente flush_events().
    uint32_t subscribe_volume(const std::string& prefab, const InterestVolume& vol,
                              std::function<void(uint32_t, bool)> cb);

    // (#9) Igual, pero solo cuenta entidades marcadas Replicated (relevancia
    // de red automática: qué replicar a este suscriptor).
    uint32_t subscribe_replicated_volume(const std::string& prefab, const InterestVolume& vol,
                                         std::function<void(uint32_t, bool)> cb);

    void unsubscribe(uint32_t sub_id);

    // (#9) Volumen de interés MÓVIL (AOI/frustum de cámara): se re-evalúa en
    // el próximo refresh_volumes() usando el índice espacial.
    void update_volume(uint32_t sub_id, const InterestVolume& vol);

    // (#9) Re-evalúa los volúmenes marcados (móviles o recién suscritos):
    // diff contra el estado actual y encola enter/leave en el batch.
    // O(candidatos del índice), no O(entidades totales).
    void refresh_volumes();

    // (#9) Entrega los batches: los callbacks se invocan una vez por network
    // tick con todos los eventos acumulados de su suscripción.
    void flush_events();

    // (#9) Relevancia actual del suscriptor (qué entidades replicarle —
    // entrada directa para el delta engine #7).
    const std::unordered_set<uint32_t>& relevant_entities(uint32_t sub_id) const;

    // (#10) Centro del volumen de interés del suscriptor (para puntuar la
    // distancia (entity, subscriber) en el LOD). False si no existe.
    bool volume_center(uint32_t sub_id, float& x, float& y, float& z) const;

    // (#10) Diff de replicación del suscriptor: enter = entidades relevantes
    // que aún no conoce (SPAWN), leave = las que dejó de conocer (DESPAWN).
    // Actualiza `known` para el próximo diff.
    void replication_diff(uint32_t sub_id, std::vector<uint32_t>& out_enter,
                          std::vector<uint32_t>& out_leave);

    // (#10) Una entidad desapareció del mundo: sacarla de la relevancia de
    // todas las suscripciones y encolar LEAVE (el suscriptor la DESPAWNeará).
    void notify_entity_despawned(uint32_t entity);

    // Called by the engine whenever an entity moves. Evalúa el nuevo punto
    // contra todas las suscripciones y ENCOLA enter/leave (batch #9).
    void notify_entity_moved(uint32_t e, const std::string& prefab,
                             float old_x, float old_y, float old_z,
                             float new_x, float new_y, float new_z);

private:
    uint32_t do_subscribe(const std::string& prefab, const InterestVolume& vol,
                          bool replicated_only, std::function<void(uint32_t, bool)> cb);

    QuerySubscription* find_sub(uint32_t sub_id);
    const QuerySubscription* find_sub(uint32_t sub_id) const;

    // Diff contra el conjunto `inside` + encola el evento en el batch.
    void apply_enter_leave(QuerySubscription& sub, uint32_t e, bool now_inside);

    uint32_t next_id_ = 1;
    mutable std::shared_mutex mutex_;
    std::vector<QuerySubscription> subscriptions_;
    EntityQueryFn query_fn_;
    ReplicatedCheckFn replicated_check_fn_;
};

} // namespace query
} // namespace fluxdb
