#include "../headers/storage.h"
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>

namespace fluxdb {
namespace storage {

namespace {

// Formato binario de un LogRecord en disco:
// lsn(8) txn_id(4) type(1) page_id(4) offset(4) size(4) old_data(256) new_data(256)
constexpr size_t RECORD_HEADER_BYTES = 8 + 4 + 1 + 4 + 4 + 4;
constexpr size_t RECORD_BYTES = RECORD_HEADER_BYTES + 256 + 256;

void serialize_record(const LogRecord& rec, char* out) {
    size_t off = 0;
    std::memcpy(out + off, &rec.lsn, 8); off += 8;
    std::memcpy(out + off, &rec.txn_id, 4); off += 4;
    std::memcpy(out + off, &rec.type, 1); off += 1;
    std::memcpy(out + off, &rec.page_id, 4); off += 4;
    std::memcpy(out + off, &rec.offset, 4); off += 4;
    std::memcpy(out + off, &rec.size, 4); off += 4;
    std::memcpy(out + off, rec.old_data, 256); off += 256;
    std::memcpy(out + off, rec.new_data, 256); off += 256;
}

void deserialize_record(const char* in, LogRecord& rec) {
    size_t off = 0;
    std::memcpy(&rec.lsn, in + off, 8); off += 8;
    std::memcpy(&rec.txn_id, in + off, 4); off += 4;
    std::memcpy(&rec.type, in + off, 1); off += 1;
    std::memcpy(&rec.page_id, in + off, 4); off += 4;
    std::memcpy(&rec.offset, in + off, 4); off += 4;
    std::memcpy(&rec.size, in + off, 4); off += 4;
    std::memcpy(rec.old_data, in + off, 256); off += 256;
    std::memcpy(rec.new_data, in + off, 256); off += 256;
}

} // namespace

LogManager::LogManager(const std::string& log_filepath) : filepath_(log_filepath) {
    file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("LogManager: no se pudo abrir el WAL: " + log_filepath);
    }

    file_.seekg(0, std::ios::end);
    std::streampos size = file_.tellg();
    next_lsn_ = 1 + static_cast<LSN>(size / RECORD_BYTES);
}

LogManager::~LogManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

LSN LogManager::append_record(const LogRecord& record) {
    std::lock_guard<std::mutex> lock(latch_);

    LogRecord rec = record;
    rec.lsn = next_lsn_++;

    char buffer[RECORD_BYTES];
    serialize_record(rec, buffer);

    file_.seekp(0, std::ios::end);
    file_.write(buffer, RECORD_BYTES);
    return rec.lsn;
}

void LogManager::flush(LSN lsn) {
    std::lock_guard<std::mutex> lock(latch_);
    file_.flush();
}

void LogManager::flush_all() {
    std::lock_guard<std::mutex> lock(latch_);
    file_.flush();
}

std::vector<LogRecord> LogManager::read_all_records() {
    std::lock_guard<std::mutex> lock(latch_);

    std::vector<LogRecord> records;
    char buffer[RECORD_BYTES];

    file_.seekg(0, std::ios::beg);
    while (true) {
        file_.read(buffer, RECORD_BYTES);
        if (file_.gcount() != static_cast<std::streamsize>(RECORD_BYTES)) {
            break;
        }

        LogRecord rec;
        deserialize_record(buffer, rec);
        records.push_back(rec);
    }

    return records;
}

RecoveryManager::RecoveryManager(std::shared_ptr<LogManager> log_mgr, BufferPoolManager* bpm)
    : log_mgr_(std::move(log_mgr)), bpm_(bpm) {}

void RecoveryManager::recover() {
    // Escanea el WAL: aplica los registros de transacciones COMMITeadas y
    // descarta (undo) los de transacciones que nunca commitearon.
    // Los registros sin txn (txn_id == 0) se tratan como autocommit.
    std::vector<LogRecord> records = log_mgr_->read_all_records();

    // Track de txns activas: txn_id -> lista de registros de datos
    std::unordered_map<uint32_t, std::vector<LogRecord*>> pending;
    std::set<uint32_t> committed;

    for (LogRecord& rec : records) {
        switch (rec.type) {
            case LogRecordType::BEGIN_TXN:
                pending[rec.txn_id];
                break;
            case LogRecordType::COMMIT_TXN:
                committed.insert(rec.txn_id);
                break;
            case LogRecordType::ABORT_TXN:
                pending.erase(rec.txn_id);
                committed.erase(rec.txn_id);
                break;
            case LogRecordType::INSERT:
            case LogRecordType::UPDATE:
            case LogRecordType::DELETE:
                if (rec.txn_id == 0) {
                    committed.insert(0);
                    pending[0].push_back(&rec);
                } else {
                    pending[rec.txn_id].push_back(&rec);
                }
                break;
        }
    }

    // Replay: solo transacciones commitadas (o autocommit)
    for (const auto& [txn_id, recs] : pending) {
        if (committed.count(txn_id) == 0) {
            continue; // Undo: no se aplica nada de esta txn
        }

        for (LogRecord* rec : recs) {
            if (rec->page_id == INVALID_PAGE || rec->size == 0) {
                continue;
            }

            size_t copy_size = (rec->size > 256) ? 256 : rec->size;
            Page* page = bpm_->fetch_page(rec->page_id);
            if (!page) {
                continue;
            }

            std::memcpy(page->data + rec->offset, rec->new_data, copy_size);
            bpm_->unpin_page(rec->page_id, true);
        }
    }
}

} // namespace storage
} // namespace fluxdb
