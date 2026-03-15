#pragma once

#include <thread>
#include <atomic>
#include <memory>
#include "storage.h"

namespace fluxdb {
namespace storage {

class VacuumManager {
public:
    VacuumManager(std::shared_ptr<PageManager> disk_mgr, std::shared_ptr<BufferPoolManager> buffer_pool);
    ~VacuumManager();

    // Inicia el hilo en background
    void start(int interval_seconds = 10);
    
    // Frena el hilo de manera limpia
    void stop();

private:
    void vacuum_loop();
    void compact_disk();

    std::shared_ptr<PageManager> disk_mgr_;
    std::shared_ptr<BufferPoolManager> buffer_pool_;
    
    std::thread worker_;
    std::atomic<bool> is_running_{false};
    int interval_seconds_ = 10;
};

} // namespace storage
} // namespace fluxdb
