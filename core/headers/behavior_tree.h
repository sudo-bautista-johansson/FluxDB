#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #16: Native Behavior Tree / GOAP Node Storage
//  (AI stack)
// ─────────────────────────────────────────────────────────────
// Un árbol de comportamiento es un array contiguo de nodos DSO. La
// *definición* es compartida (data-driven, versionada) y los agentes
// solo guardan un ref + un estado de ejecución diminuto. El batch
// tic de todos los agentes que comparten la misma definición mejora
// la localidad de caché y el predictor de ramas. GOAP produce planes
// como secuencias de acciones compactas.

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <memory>

namespace fluxdb {
namespace ai {

enum class BTNodeType : uint8_t {
    SELECTOR,  // suc2ce al primer hijo que succeede
    SEQUENCE,  // fracasa al primer hijo que falla
    CONDITION, // evalúa una condición del blackboard
    ACTION     // ejecuta una acción (opcional)
};

enum class BTStatus : uint8_t {
    IDLE,
    RUNNING,
    SUCCESS,
    FAILURE
};

// Nodo compacto. `child_begin`/`child_count` indexan el mismo array
// para nodos compuestos; `param` es un valor configurable de la
// definición (p.ej. id de blackboard a consultar para CONDITION).
struct BTNode {
    BTNodeType type = BTNodeType::ACTION;
    BTStatus status = BTStatus::IDLE;
    uint16_t child_begin = 0;
    uint16_t child_count = 0;
    uint32_t param = 0;

    BTNode() = default;
    BTNode(BTNodeType t) : type(t) {}
};

// Definición compartida de árbol: array de nodos + versión.
// Dos agentes con la MISMA topología comparten esta estructura.
struct BehaviorTreeDef {
    uint64_t id = 0;
    uint32_t version = 0;
    std::vector<BTNode> nodes;

    BehaviorTreeDef() = default;
    explicit BehaviorTreeDef(uint64_t tree_id) : id(tree_id) {}

    // Valida referencias de hijos y estructura.
    bool validate() const {
        if (nodes.empty()) return false;
        for (const auto& n : nodes) {
            if (n.child_count == 0) continue;
            uint64_t end = static_cast<uint64_t>(n.child_begin) + n.child_count;
            if (end > nodes.size()) return false;
        }
        return true;
    }
};

// Estado de ejecución PER-AGENTE (diminuto).
struct BehaviorState {
    int32_t current = 0;        // nodo actual
    uint64_t tree_id = 0;       // a qué definición pertenece
    uint32_t last_version = 0;  // para hot-reload (re-validar en cambio)
    int32_t subtree[8] = {};    // índices de sub-árbol en evaluación
};

// Registro de definiciones compartidas (data-driven, hot-reloadable).
class BehaviorTreeRegistry {
public:
    uint64_t register_def(BehaviorTreeDef def) {
        if (!def.validate()) return 0;
        uint64_t id = next_id_++;
        def.id = id;
        defs_[id] = std::move(def);
        return id;
    }

    BehaviorTreeDef* get(uint64_t id) {
        auto it = defs_.find(id);
        return it == defs_.end() ? nullptr : &it->second;
    }

    const BehaviorTreeDef* get(uint64_t id) const {
        auto it = defs_.find(id);
        return it == defs_.end() ? nullptr : &it->second;
    }

    // Hot-reload: sube la versión de una definición existente.
    bool bump_version(uint64_t id) {
        auto it = defs_.find(id);
        if (it == defs_.end()) return false;
        ++it->second.version;
        return true;
    }

    size_t count() const { return defs_.size(); }

private:
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, BehaviorTreeDef> defs_;
};

// Evaluación determinista de un nodo compuesto.
// `tick(node)` es un callback de usuario que ejecuta la lógica
// (lea del blackboard, etc.) y devuelve RUNNING/SUCCESS/FAILURE.
class BehaviorTreeEvaluator {
public:
    // Ejecuta un tick completo sobre el estado dado. Actualiza el estado
    // en el lugar (determinista, batch-friendly). Devuelve el estado del root.
    BTStatus tick(const BehaviorTreeDef& def, BehaviorState& st) const {
        if (!def.validate() || st.current >= static_cast<int32_t>(def.nodes.size())) {
            return BTStatus::FAILURE;
        }
        return eval(def, st, st.current);
    }

private:
    BTStatus eval(const BehaviorTreeDef& def, BehaviorState& st, int32_t node_idx) const {
        const BTNode& node = def.nodes[node_idx];
        switch (node.type) {
        case BTNodeType::SELECTOR: {
            for (uint16_t c = 0; c < node.child_count; ++c) {
                BTStatus s = eval(def, st, node.child_begin + c);
                if (s == BTStatus::SUCCESS) return BTStatus::SUCCESS;
                if (s == BTStatus::RUNNING) return BTStatus::RUNNING;
            }
            return BTStatus::FAILURE;
        }
        case BTNodeType::SEQUENCE: {
            for (uint16_t c = 0; c < node.child_count; ++c) {
                BTStatus s = eval(def, st, node.child_begin + c);
                if (s == BTStatus::FAILURE) return BTStatus::FAILURE;
                if (s == BTStatus::RUNNING) return BTStatus::RUNNING;
            }
            return BTStatus::SUCCESS;
        }
        default:
            // CONDITION / ACTION: el hook de usuario.
            return hook_(def, st, node);
        }
    }

public:
    // Hook por defecto: los nodos hoja se tratan como SUCCESS salvo que
    // el usuario los implemente. Las condiciones usan `param` como id de
    // key de blackboard si el usuario registró un resolver.
    using NodeHook = BTStatus (*)(const BehaviorTreeDef&, BehaviorState&, const BTNode&);
    NodeHook hook_ = [](const BehaviorTreeDef&, BehaviorState&, const BTNode&) {
        return BTStatus::SUCCESS;
    };

    void set_hook(NodeHook h) { hook_ = h; }
};

// ── GOAP: almacenamiento de planes ───────────────────────────

struct GOAPActionDef {
    uint64_t id = 0;
    float cost = 1.0f;
    // efectos/requisitos como pares key→valor tipado genérico (simplif.).
    int32_t effect_key = 0;
    int32_t effect_value = 0;
    int32_t precondition_key = 0;
    int32_t precondition_value = 0;
    bool precondition_negated = false;
};

// Estado de mundo del planificador: pares key→int.
struct GOAPState {
    int32_t values[8] = {};
    bool valid[8] = {};
    int32_t keys[8] = {};

    void set(int32_t key, int32_t value) {
        for (int i = 0; i < 8; ++i) {
            if (valid[i] && keys[i] == key) { values[i] = value; return; }
            if (!valid[i]) { valid[i] = true; keys[i] = key; values[i] = value; return; }
        }
    }
    bool satisfies(int32_t key, int32_t value, bool negated) const {
        for (int i = 0; i < 8; ++i) {
            if (valid[i] && keys[i] == key) {
                bool eq = values[i] == value;
                return negated ? !eq : eq;
            }
        }
        return negated; // key ausente: negada = true, positiva = false
    }
};

// Un plan es una secuencia compacta de acciones.
struct GOAPPlan {
    uint64_t action_ids[8] = {};
    uint8_t count = 0;
};

// Planificador por fuerza bruta con A* simple sobre estados (8 slots).
// Suficiente para planes cortos de IA de RTS/accion.
class GOAPlanner {
public:
    GOAPlanner() = default;

    void add_action(const GOAPActionDef& a) { actions_.push_back(a); }
    const std::vector<GOAPActionDef>& actions() const { return actions_; }

    // Encuentra un plan desde `start` hacia el estado objetivo `goal`
    // (pares key→valor). Devuelve false si no hay plan (limite nodos).
    bool plan(const GOAPState& start, const GOAPState& goal, GOAPPlan& out, int max_nodes = 256) const {
        // Backward search simple: desde el goal, retrocede aplicando
        // precondiciones como sub-objetivos hasta cubrir el estado inicial.
        // Implementación greedy con límite de nodos (demo determinista).
        GOAPState current = goal;
        int visited = 0;
        while (visited < max_nodes) {
            // ¿El estado inicial ya cubre el estado actual?
            if (covers(start, current)) {
                reverse(out); // el plan se construyó en orden inverso
                return true;
            }
            // Busca una acción cuyo efecto aproxime el estado actual.
            bool progressed = false;
            for (const auto& a : actions_) {
                if (approx(a, current, start)) {
                    append_action(out, a.id);
                    apply_effect_backwards(a, current);
                    progressed = true;
                    ++visited;
                    break;
                }
            }
            if (!progressed) return false;
        }
        return false;
    }

private:
    std::vector<GOAPActionDef> actions_;

    static void append_action(GOAPPlan& p, uint64_t id) {
        if (p.count < 8) p.action_ids[p.count++] = id;
    }
    static void reverse(GOAPPlan& p) {
        for (uint8_t i = 0; i < p.count / 2; ++i) {
            std::swap(p.action_ids[i], p.action_ids[p.count - 1 - i]);
        }
    }
    static bool covers(const GOAPState& have, const GOAPState& want) {
        for (int i = 0; i < 8; ++i) {
            if (want.valid[i] && !have.satisfies(want.keys[i], want.values[i], false)) return false;
        }
        return true;
    }
    // ¿La acción es candidata? Su efecto mueve el estado actual hacia el
    // objetivo (el estado "aún por cumplir" del backward search). Las
    // precondiciones NO se comprueban aquí: se convierten en sub-objetivos
    // vía apply_effect_backwards, típico de la regresión de GOAP.
    bool approx(const GOAPActionDef& a, const GOAPState& current, const GOAPState&) const {
        if (a.effect_key == 0) return false;
        // El efecto debe apuntar a un slot que el objetivo quiera.
        for (int i = 0; i < 8; ++i) {
            if (current.valid[i] && current.keys[i] == a.effect_key) return true;
        }
        return false;
    }
    static void apply_effect_backwards(const GOAPActionDef& a, GOAPState& current) {
        // Regresión: el efecto se cumple, queda por cumplir la precondición.
        for (int i = 0; i < 8; ++i) {
            if (current.valid[i] && current.keys[i] == a.effect_key) {
                current.valid[i] = false; // lo damos por logrado vía acción
            }
        }
        if (a.precondition_key != 0) {
            current.set(a.precondition_key, a.precondition_value);
        }
    }
};

} // namespace ai
} // namespace fluxdb
