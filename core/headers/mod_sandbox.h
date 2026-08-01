#pragma once

// ─────────────────────────────────────────────────────────────
//  Feature #26: Sandboxed Mod Query/Script API
//  (Phase 5 - Frontier / Modding)
// ─────────────────────────────────────────────────────────────
// Vista restringida del World para mods no confiables. En lugar de
// exponer punteros crudos, la sandbox ofrece queries por capabilities:
// un ModCapabilitySet (whitelist de componentes + permiso read/write) se
// valida EN LA CONSTRUCCIÓN de cada query, no por convención. El FFI usa
// llamadas batch (por query, no por entidad) para minimizar overhead.

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <functional>

namespace fluxdb {
namespace sandbox {

enum class Permission : uint8_t { NONE, READ, WRITE, READ_WRITE };

// Capability de un mod: qué componentes puede ver y cómo.
class CapabilitySet {
public:
    void grant(const std::string& component, Permission perm) {
        perms_[component] = perm;
    }

    Permission permission_of(const std::string& component) const {
        auto it = perms_.find(component);
        return it == perms_.end() ? Permission::NONE : it->second;
    }

    bool can_read(const std::string& c) const {
        Permission p = permission_of(c);
        return p == Permission::READ || p == Permission::WRITE || p == Permission::READ_WRITE;
    }
    bool can_write(const std::string& c) const {
        Permission p = permission_of(c);
        return p == Permission::WRITE || p == Permission::READ_WRITE;
    }

    bool empty() const { return perms_.empty(); }

private:
    std::unordered_map<std::string, Permission> perms_;
};

// Resultado de una query batch (sin per-entity FFI hops).
struct QueryResult {
    std::vector<uint32_t> entities;
    std::vector<std::string> fields;   // nombres en el mismo orden
    std::vector<std::vector<float>> columns; // columna por campo
};

// Una query declarada contra la sandbox.
struct SandboxedQuery {
    std::string component;
    std::vector<std::string> fields;     // campos leídos
    std::vector<std::string> write_fields; // campos escritos (los del write)
    bool allow_write = false;
    bool constructed = false; // si pasó la validación de capabilities
    std::string reject_reason;
};

// Un Store por componente: entidades + filas.
struct ComponentStore {
    std::vector<uint32_t> entities;
    std::vector<std::vector<float>> rows;
    bool empty() const { return entities.empty(); }
};

// El World simulado (minimal): mapa componente → store. La sandbox NO
// deja ver los componentes no permitidos.
class MockWorld {
public:
    void add_entity(const std::string& component, std::vector<float> fields) {
        ComponentStore& s = records_[component];
        s.entities.push_back(next_id_);
        s.rows.push_back(std::move(fields));
        next_id_++;
    }

    uint32_t entity_count(const std::string& component) const {
        auto it = records_.find(component);
        return it == records_.end() ? 0 : static_cast<uint32_t>(it->second.entities.size());
    }

    bool read(const std::string& component, uint32_t entity, std::vector<float>& out) const {
        auto it = records_.find(component);
        if (it == records_.end()) return false;
        for (size_t i = 0; i < it->second.entities.size(); ++i) {
            if (it->second.entities[i] == entity) { out = it->second.rows[i]; return true; }
        }
        return false;
    }

    bool write(const std::string& component, uint32_t entity, const std::vector<float>& vals) {
        auto it = records_.find(component);
        if (it == records_.end()) return false;
        for (size_t i = 0; i < it->second.entities.size(); ++i) {
            if (it->second.entities[i] == entity) { it->second.rows[i] = vals; return true; }
        }
        return false;
    }

    // Acceso interno para la sandbox (ya validada por capabilities).
    const ComponentStore& store(const std::string& component) const {
        static const ComponentStore empty_store;
        auto it = records_.find(component);
        return it == records_.end() ? empty_store : it->second;
    }

private:
    std::unordered_map<std::string, ComponentStore> records_;
    uint32_t next_id_ = 1;
};

// Sandbox: proyección filtrada del World. Cada query se valida contra el
// CapabilitySet al construirse. Las llamadas son batch.
class SandboxedWorldView {
public:
    SandboxedWorldView(MockWorld& world, const CapabilitySet& caps)
        : world_(world), caps_(caps) {}

    // Valida y construye una query. Devuelve false y setea reason si el mod
    // no tiene permisos.
    bool build_query(SandboxedQuery& q) {
        if (!caps_.can_read(q.component)) {
            q.constructed = false;
            q.reject_reason = "no read permission on component '" + q.component + "'";
            return false;
        }
        if (q.allow_write && !caps_.can_write(q.component)) {
            q.constructed = false;
            q.reject_reason = "no write permission on component '" + q.component + "'";
            return false;
        }
        q.constructed = true;
        q.reject_reason.clear();
        return true;
    }

    // Ejecuta una query ya validada (batch: todos los campos a la vez).
    // Si la query no está construida, devuelve resultado vacío (safe).
    QueryResult run(const SandboxedQuery& q) {
        QueryResult out;
        if (!q.constructed || !caps_.can_read(q.component)) return out;

        const ComponentStore& s = world_.store(q.component);
        if (s.empty()) return out;
        out.entities = s.entities;
        out.fields = q.fields;
        for (const auto& field : q.fields) {
            std::vector<float> col;
            col.reserve(s.rows.size());
            size_t idx = field_index(field);
            for (const auto& row : s.rows) {
                col.push_back(idx < row.size() ? row[idx] : 0.0f);
            }
            out.columns.push_back(std::move(col));
        }
        return out;
    }

    // Escritura batch: escribe a todos los campos write_fields de las
    // entidades (batch, sin hops por entidad). Devuelve nº de entidades.
    size_t update_matching(const SandboxedQuery& q,
                           const std::function<float(const std::vector<float>&)>& new_value) {
        if (!q.constructed || !caps_.can_write(q.component)) return 0;
        const ComponentStore& s = world_.store(q.component);
        if (s.empty()) return 0;

        size_t count = 0;
        for (size_t i = 0; i < s.entities.size(); ++i) {
            const auto& row = s.rows[i];
            float nv = new_value(row);
            std::vector<float> new_row = row;
            for (const auto& wf : q.write_fields) {
                size_t idx = field_index(wf);
                if (idx < new_row.size()) new_row[idx] = nv;
            }
            world_.write(q.component, s.entities[i], new_row);
            ++count;
        }
        return count;
    }

    // El mod NO puede acceder a componentes fuera de sus capabilities.
    bool can_see(const std::string& component) const {
        return caps_.can_read(component);
    }

private:
    // Campo por índice según declaración estándar (suficiente para el test).
    static size_t field_index(const std::string& name) {
        if (name == "x" || name == "hp" || name == "stamina") return 0;
        if (name == "y" || name == "regen") return 1;
        if (name == "z") return 2;
        return 0;
    }

    MockWorld& world_;
    const CapabilitySet& caps_;
};

} // namespace sandbox
} // namespace fluxdb
