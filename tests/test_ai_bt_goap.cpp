// FluxDB — Feature #16: Native Behavior Tree / GOAP Node Storage
// Definiciones de árbol compartidas + estado por agente + GOAP plano.
#include "../core/headers/behavior_tree.h"
#include "../core/headers/blackboard.h"
#include <iostream>
#include <cassert>
#include <cmath>

using namespace fluxdb::ai;

static int checks = 0;
#define CHECK(cond) do { assert(cond); ++checks; } while (0)

static void test_tree_registry_and_validation() {
    std::cout << "1. Registry de definiciones y validación...\n";

    BehaviorTreeRegistry reg;

    // Árbol: selector → sequence(salud alta) + action(huir)
    BehaviorTreeDef def;
    BTNode selector(BTNodeType::SELECTOR);
    selector.child_begin = 1; selector.child_count = 2;
    BTNode seq(BTNodeType::SEQUENCE);
    seq.child_begin = 3; seq.child_count = 2;
    BTNode cond_high(BTNodeType::CONDITION);
    cond_high.param = 1001;
    BTNode act_attack(BTNodeType::ACTION);
    BTNode act_flee(BTNodeType::ACTION);

    def.nodes.push_back(selector);      // 0
    def.nodes.push_back(seq);           // 1
    def.nodes.push_back(act_flee);      // 2
    def.nodes.push_back(cond_high);     // 3
    def.nodes.push_back(act_attack);    // 4

    CHECK(def.validate());
    uint64_t id = reg.register_def(def);
    CHECK(id != 0);
    CHECK(reg.count() == 1);

    // Definición inválida: hijo fuera de rango.
    BehaviorTreeDef bad;
    BTNode bad_sel(BTNodeType::SELECTOR);
    bad_sel.child_begin = 5; bad_sel.child_count = 3;
    bad.nodes.push_back(bad_sel);
    CHECK(!bad.validate());
    CHECK(reg.register_def(bad) == 0);
    CHECK(reg.count() == 1);

    // Hot-reload: bump de versión.
    CHECK(reg.bump_version(id));
    const BehaviorTreeDef* got = reg.get(id);
    CHECK(got != nullptr);
    CHECK(got->version == 1);
    CHECK(!reg.bump_version(9999));
}

static BTStatus check_hook(const BehaviorTreeDef& def, BehaviorState& st, const BTNode& node) {
    if (node.type == BTNodeType::CONDITION) {
        // Condición "salud alta": param es la key del blackboard.
        return node.param == 1001 ? BTStatus::SUCCESS : BTStatus::FAILURE;
    }
    if (node.type == BTNodeType::ACTION) {
        return BTStatus::SUCCESS;
    }
    return BTStatus::SUCCESS;
}

static void test_tree_evaluation_shared_def() {
    std::cout << "2. Evaluación determinista con definición compartida...\n";

    BehaviorTreeRegistry reg;
    BehaviorTreeDef def;
    BTNode selector(BTNodeType::SELECTOR);
    selector.child_begin = 1; selector.child_count = 2;
    BTNode seq(BTNodeType::SEQUENCE);
    seq.child_begin = 3; seq.child_count = 2;
    BTNode cond_high(BTNodeType::CONDITION);
    cond_high.param = 1001;
    BTNode act_attack(BTNodeType::ACTION);
    BTNode act_flee(BTNodeType::ACTION);
    def.nodes.push_back(selector);
    def.nodes.push_back(seq);
    def.nodes.push_back(act_flee);
    def.nodes.push_back(cond_high);
    def.nodes.push_back(act_attack);
    uint64_t id = reg.register_def(def);

    BehaviorTreeEvaluator eval;
    eval.set_hook(check_hook);

    // Dos agentes comparten la MISMA definición (batch).
    BehaviorState st1; st1.tree_id = id; st1.current = 0;
    BehaviorState st2; st2.tree_id = id; st2.current = 0;

    BTStatus r1 = eval.tick(def, st1);
    BTStatus r2 = eval.tick(def, st2);
    CHECK(r1 == BTStatus::SUCCESS);
    CHECK(r2 == BTStatus::SUCCESS);
}

static void test_tree_hot_reload_revalidation() {
    std::cout << "3. Hot-reload: bump de versión invalida el estado viejo...\n";

    BehaviorTreeRegistry reg;
    BehaviorTreeDef def;
    BTNode act(BTNodeType::ACTION);
    def.nodes.push_back(act);
    uint64_t id = reg.register_def(def);

    BehaviorState st;
    st.tree_id = id;
    st.last_version = reg.get(id)->version;

    reg.bump_version(id);
    CHECK(reg.get(id)->version != st.last_version);
    st.last_version = reg.get(id)->version;
    CHECK(st.last_version == reg.get(id)->version);
}

static void test_goap_planning() {
    std::cout << "4. GOAP: planificación backward desde el goal...\n";

    GOAPlanner planner;

    // Acciones:
    //  A: "recolectar madera" — pre: nada, efecto: madera=1
    GOAPActionDef gather;
    gather.id = 1; gather.cost = 1.0f;
    gather.effect_key = 10; gather.effect_value = 1;
    gather.precondition_key = 0; // sin precondición
    planner.add_action(gather);

    //  B: "construir casa" — pre: madera=1, efecto: casa=1
    GOAPActionDef build;
    build.id = 2; build.cost = 2.0f;
    build.effect_key = 11; build.effect_value = 1;
    build.precondition_key = 10; build.precondition_value = 1;
    planner.add_action(build);

    CHECK(planner.actions().size() == 2);

    // Estado inicial vacío → meta: casa=1.
    GOAPState start;
    GOAPState goal;
    goal.set(11, 1);

    GOAPPlan plan;
    CHECK(planner.plan(start, goal, plan));
    CHECK(plan.count >= 1);
    // El plan debe terminar con "construir casa" (acción 2).
    CHECK(plan.action_ids[plan.count - 1] == 2);
    // La recolecta (1) debe preceder si está en el plan.
    for (uint8_t i = 0; i < plan.count; ++i) {
        CHECK(plan.action_ids[i] == 1 || plan.action_ids[i] == 2);
    }

    // Plan imposible: meta inalcanzable.
    GOAPState goal2;
    goal2.set(12, 1); // no hay acción que genere key 12
    GOAPPlan plan2;
    CHECK(!planner.plan(start, goal2, plan2));
}

int main() {
    std::cout << "--- Starting FluxDB Behavior Tree / GOAP Test (#16) ---\n";
    test_tree_registry_and_validation();
    test_tree_evaluation_shared_def();
    test_tree_hot_reload_revalidation();
    test_goap_planning();
    std::cout << "--- BEHAVIOR TREE / GOAP TEST PASSED (" << checks << " checks) ---\n";
    return 0;
}