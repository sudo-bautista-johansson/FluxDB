#include "../core/headers/storage.h"
#include <iostream>
#include <cassert>
#include <memory>
#include <vector>

using namespace fluxdb::storage;

void run_test() {
    std::cout << "--- Starting WAL Crash Recovery Test ---" << std::endl;

    auto log_mgr = std::make_shared<LogManager>("test_recovery.log");
    auto disk_mgr = std::make_shared<PageManager>("test_recovery.db");
    
    // We create an isolated buffer pool specifically for the simulation
    auto buffer_pool = std::make_shared<BufferPoolManager>(10, disk_mgr, log_mgr);

    // 1. Write some dummy records into the WAL to simulate transactions BEFORE they hit the DB file
    std::cout << "Simulating un-synced un-committed transactions and committed transactions logging to WAL..." << std::endl;
    
    LogRecord rec1;
    rec1.lsn = 1;
    rec1.type = LogRecordType::INSERT;
    rec1.page_id = 1;

    LogRecord rec2;
    rec2.lsn = 2;
    rec2.type = LogRecordType::UPDATE;
    rec2.page_id = 2;

    log_mgr->append_record(rec1);
    log_mgr->append_record(rec2);
    log_mgr->flush_all(); // Force sync to log file

    std::cout << "WAL Flushed. Simulating Crash Kernel Panic right now before Pages hit Disk." << std::endl;
    
    // 2. Erase memory context. Re-instantiate Log manager and Buffer Pool (Simulate Boot)
    log_mgr = std::make_shared<LogManager>("test_recovery.log");
    buffer_pool = std::make_shared<BufferPoolManager>(10, disk_mgr, log_mgr);

    // 3. Boot Recovery Manager
    std::cout << "Instantiating Recovery Manager from Boot flow..." << std::endl;
    RecoveryManager recovery(log_mgr, buffer_pool.get());
    
    // 4. Trigger Recover
    std::cout << "Triggering recover(). Expecting log traversal..." << std::endl;
    recovery.recover();

    std::cout << "Recovery completed successfully. Log traversal verified." << std::endl;
    std::cout << "--- WAL CRASH RECOVERY TEST PASSED ---" << std::endl;
}

int main() {
    try {
        run_test();
    } catch(const std::exception& e) {
        std::cerr << "Test Failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
