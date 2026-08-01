#include "../headers/storage.h"
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace fluxdb {
namespace storage {

PageManager::PageManager(const std::string& filepath) : filepath_(filepath) {
    file_.open(filepath_, std::ios::binary | std::ios::in | std::ios::out | std::ios::app);
    if (!file_.is_open()) {
        throw std::runtime_error("PageManager: no se pudo abrir el archivo: " + filepath_);
    }

    file_.seekg(0, std::ios::end);
    std::streampos size = file_.tellg();
    num_pages_ = (size > 0) ? static_cast<size_t>(size) / PAGE_SIZE : 0;
}

PageManager::~PageManager() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

void PageManager::read_page(PageID page_id, char* page_data) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (page_id >= num_pages_) {
        // Página nunca escrita (p.ej. replay de WAL antes de que la página exista)
        std::memset(page_data, 0, PAGE_SIZE);
        return;
    }

    file_.seekg(static_cast<std::streamoff>(page_id) * PAGE_SIZE, std::ios::beg);
    file_.read(page_data, PAGE_SIZE);
    if (file_.gcount() != static_cast<std::streamsize>(PAGE_SIZE)) {
        std::memset(page_data, 0, PAGE_SIZE);
    }
}

void PageManager::write_page(PageID page_id, const char* page_data) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    file_.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE, std::ios::beg);
    file_.write(page_data, PAGE_SIZE);
    file_.flush();
}

PageID PageManager::allocate_page() {
    std::lock_guard<std::mutex> lock(io_mutex_);
    num_pages_++;
    return static_cast<PageID>(num_pages_ - 1);
}

} // namespace storage
} // namespace fluxdb
