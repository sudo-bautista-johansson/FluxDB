#pragma once

#include <string>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <memory>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <cstring>

namespace fluxdb {
namespace storage {

// Constantes globales de disco
constexpr size_t PAGE_SIZE = 4096;      // 4KB por página
using PageID = uint32_t;
using LSN = uint64_t;
constexpr PageID INVALID_PAGE = UINT32_MAX;
constexpr LSN INVALID_LSN = 0;

// ─────────────────────────────────────────
//  Write-Ahead Log (WAL) Structures
// ─────────────────────────────────────────

enum class LogRecordType : uint8_t {
    INSERT = 0,
    UPDATE,
    DELETE,
    BEGIN_TXN,
    COMMIT_TXN,
    ABORT_TXN
};

struct LogRecord {
    LSN lsn = INVALID_LSN;
    uint32_t txn_id = 0;
    LogRecordType type = LogRecordType::BEGIN_TXN;
    PageID page_id = INVALID_PAGE;
    
    // For simplicity in this demo WAL, we store raw entity diffs 
    // or just say "this page was modified by this Txn"
    uint32_t offset = 0;
    uint32_t size = 0;
    uint8_t old_data[256]; // simplistic limit for demo components
    uint8_t new_data[256];
};

class LogManager {
public:
    explicit LogManager(const std::string& log_filepath);
    ~LogManager();

    // Append a record to log buffer and return its LSN
    LSN append_record(const LogRecord& record);
    
    // Force write log buffer to disk up to the specified LSN
    void flush(LSN lsn);
    void flush_all();

private:
    std::string filepath_;
    std::fstream file_;
    std::mutex latch_;
    LSN next_lsn_ = 1;
};

class RecoveryManager {
public:
    RecoveryManager(std::shared_ptr<LogManager> log_mgr, class BufferPoolManager* bpm);
    
    // Scans the WAL files on startup to replay committed txns 
    // and undo uncommitted txns.
    void recover();

private:
    std::shared_ptr<LogManager> log_mgr_;
    class BufferPoolManager* bpm_;
};

// ─────────────────────────────────────────
//  Estructura de una Página (En Memoria)
// ─────────────────────────────────────────

struct Page {
    PageID id = INVALID_PAGE;
    bool is_dirty = false;
    int pin_count = 0;
    LSN page_lsn = INVALID_LSN; // Enterprise WAL feature
    
    // El bloque RAW de memoria mapeado de disco
    uint8_t data[PAGE_SIZE];

    Page() {
        reset();
    }

    void reset() {
        id = INVALID_PAGE;
        is_dirty = false;
        pin_count = 0;
        page_lsn = INVALID_LSN;
        std::memset(data, 0, PAGE_SIZE);
    }
};

// ─────────────────────────────────────────
//  PageManager (Manejo Duro del Disco)
// ─────────────────────────────────────────

class PageManager {
public:
    explicit PageManager(const std::string& filepath);
    ~PageManager();

    // Prevent copy
    PageManager(const PageManager&) = delete;
    PageManager& operator=(const PageManager&) = delete;

    // Lee una página desde el disco hacia el buffer de memoria
    void read_page(PageID page_id, char* page_data);

    // Escribe la página sucia desde el buffer hacia el disco
    void write_page(PageID page_id, const char* page_data);

    // Asigna/expande una página nueva y lógica al final del archivo
    PageID allocate_page();

    // Devuelve el tamaño lógico actual (cantidad de páginas)
    size_t get_num_pages() const { return num_pages_; }

private:
    std::string filepath_;
    std::fstream file_;
    size_t num_pages_ = 0;
    std::mutex io_mutex_;
};

// ─────────────────────────────────────────
//  BufferPool (LRU Cache en RAM)
// ─────────────────────────────────────────

class BufferPoolManager {
public:
    BufferPoolManager(size_t pool_size, std::shared_ptr<PageManager> disk_manager, std::shared_ptr<LogManager> log_manager = nullptr);
    ~BufferPoolManager();

    // Pide una página (la lee del disco si no está en RAM) y le sube el PIN (uso)
    Page* fetch_page(PageID page_id);

    // Baja el PIN indicando que terminamos de usarla. Se marca is_dirty si hubo writes.
    bool unpin_page(PageID page_id, bool is_dirty);

    // Fuerza flush al disco de todas las páginas dirty
    void flush_all_pages();

    // Crear una página totalmente nueva (asigna disco, carga a RAM)
    Page* new_page(PageID* out_page_id);

private:
    bool find_victim_page(Page** victim);
    
    size_t pool_size_;
    std::shared_ptr<PageManager> disk_manager_;
    std::vector<Page> pages_;

    // Mapa ID -> Indice de vector `pages_` para acceso veloz O(1)
    std::unordered_map<PageID, size_t> page_table_;

    // Para el LRU simple, mantenemos una lista enlazada de accesos o contadores
    std::vector<uint32_t> access_time_;
    uint32_t current_time_ = 0;

    std::mutex latch_;
    std::shared_ptr<LogManager> log_manager_;
};

} // namespace storage
} // namespace fluxdb
