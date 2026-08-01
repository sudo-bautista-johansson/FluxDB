#include "../headers/vacuum.h"
#include <chrono>

namespace fluxdb {
namespace storage {

VacuumManager::VacuumManager(std::shared_ptr<PageManager> disk_mgr,
                             std::shared_ptr<BufferPoolManager> buffer_pool)
    : disk_mgr_(std::move(disk_mgr)), buffer_pool_(std::move(buffer_pool)) {}

VacuumManager::~VacuumManager() {
    stop();
}

void VacuumManager::start(int interval_seconds) {
    interval_seconds_ = interval_seconds;
    if (is_running_.exchange(true)) {
        return;
    }
    worker_ = std::thread(&VacuumManager::vacuum_loop, this);
}

void VacuumManager::stop() {
    if (is_running_.exchange(false)) {
        if (worker_.joinable()) {
            worker_.join();
        }
    }
}

void VacuumManager::vacuum_loop() {
    while (is_running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds_));
        if (!is_running_.load()) {
            break;
        }
        compact_disk();
    }
}

void VacuumManager::compact_disk() {
    // Compactación básica: fuerza el flush de páginas dirty a disco.
    buffer_pool_->flush_all_pages();
}

} // namespace storage
} // namespace fluxdb
