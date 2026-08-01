#include "../headers/storage.h"
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace fluxdb {
namespace storage {

BufferPoolManager::BufferPoolManager(size_t pool_size, std::shared_ptr<PageManager> disk_manager,
                                     std::shared_ptr<LogManager> log_manager)
    : pool_size_(pool_size), disk_manager_(std::move(disk_manager)),
      log_manager_(std::move(log_manager)) {
    pages_.resize(pool_size_);
    access_time_.resize(pool_size_, 0);
}

BufferPoolManager::~BufferPoolManager() {
    flush_all_pages();
}

Page* BufferPoolManager::fetch_page(PageID page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        size_t idx = it->second;
        pages_[idx].pin_count++;
        access_time_[idx] = current_time_++;
        return &pages_[idx];
    }

    Page* victim = nullptr;
    if (!find_victim_page(&victim)) {
        return nullptr;
    }

    size_t idx = static_cast<size_t>(victim - pages_.data());

    // Si la víctima tenía una página distinta, des-registrarla
    if (victim->id != INVALID_PAGE) {
        page_table_.erase(victim->id);
        if (victim->is_dirty) {
            disk_manager_->write_page(victim->id, reinterpret_cast<const char*>(victim->data));
        }
    }

    victim->reset();
    victim->id = page_id;
    disk_manager_->read_page(page_id, reinterpret_cast<char*>(victim->data));
    victim->pin_count = 1;
    access_time_[idx] = current_time_++;
    page_table_[page_id] = idx;

    return victim;
}

bool BufferPoolManager::unpin_page(PageID page_id, bool is_dirty) {
    std::lock_guard<std::mutex> lock(latch_);

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;
    }

    Page& page = pages_[it->second];
    if (page.pin_count > 0) {
        page.pin_count--;
    }
    if (is_dirty) {
        page.is_dirty = true;
    }
    return true;
}

void BufferPoolManager::flush_all_pages() {
    std::lock_guard<std::mutex> lock(latch_);

    for (Page& page : pages_) {
        if (page.id != INVALID_PAGE && page.is_dirty) {
            disk_manager_->write_page(page.id, reinterpret_cast<const char*>(page.data));
            page.is_dirty = false;
        }
    }
}

Page* BufferPoolManager::new_page(PageID* out_page_id) {
    std::lock_guard<std::mutex> lock(latch_);

    Page* victim = nullptr;
    if (!find_victim_page(&victim)) {
        return nullptr;
    }

    size_t idx = static_cast<size_t>(victim - pages_.data());

    if (victim->id != INVALID_PAGE) {
        page_table_.erase(victim->id);
        if (victim->is_dirty) {
            disk_manager_->write_page(victim->id, reinterpret_cast<const char*>(victim->data));
        }
    }

    PageID new_id = disk_manager_->allocate_page();
    victim->reset();
    victim->id = new_id;
    victim->pin_count = 1;
    access_time_[idx] = current_time_++;
    page_table_[new_id] = idx;

    if (out_page_id) {
        *out_page_id = new_id;
    }
    return victim;
}

bool BufferPoolManager::find_victim_page(Page** victim) {
    size_t best_idx = pool_size_;
    uint32_t best_time = UINT32_MAX;

    for (size_t i = 0; i < pool_size_; ++i) {
        if (pages_[i].pin_count > 0) {
            continue;
        }
        if (access_time_[i] < best_time) {
            best_time = access_time_[i];
            best_idx = i;
        }
    }

    if (best_idx == pool_size_) {
        return false;
    }

    *victim = &pages_[best_idx];
    return true;
}

} // namespace storage
} // namespace fluxdb
