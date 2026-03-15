#include <iostream>
#include <string>
#include <cstring>
#include <cstdio>
#include "../core/headers/storage.h"

using namespace veldradb::storage;

int main() {
    std::cout << "--- VeldraDB Storage Layer Test ---\n";
    std::cout.flush();
    
    std::string test_db = "test.vdb";
    std::remove(test_db.c_str());

    try {
        std::cout << "Creating Manager...\n";
        auto disk = std::make_shared<PageManager>(test_db);
        BufferPoolManager pool(10, disk);

        PageID p0_id, p1_id;

        std::cout << "Allocating pages...\n";
        Page* page0 = pool.new_page(&p0_id);
        Page* page1 = pool.new_page(&p1_id);

        if (!page0 || !page1) throw std::runtime_error("new_page returned nullptr");
        
        std::cout << "Writing memory...\n";
        std::strcpy(reinterpret_cast<char*>(page0->data), "VeldraDB Master Record");
        std::strcpy(reinterpret_cast<char*>(page1->data), "Page 1 Content");

        pool.unpin_page(p0_id, true);
        pool.unpin_page(p1_id, true);
        
        std::cout << "Scope destroying (flushing)...\n";
    } catch (const std::exception& e) {
        std::cerr << "Exception 1: " << e.what() << "\n";
        return 1;
    }

    std::cout << "✓ RAM -> Disk Writes persist successfully\n";

    try {
        std::cout << "Re-opening Manager...\n";
        auto disk = std::make_shared<PageManager>(test_db);
        BufferPoolManager pool(10, disk);

        std::cout << "Fetching page 0...\n";
        Page* page0 = pool.fetch_page(0);
        
        std::string raw(reinterpret_cast<char*>(page0->data));
        std::cout << "Fetched: " << raw << "\n";
        if (raw != "VeldraDB Master Record") throw std::runtime_error("Data mismatch");
        pool.unpin_page(0, false);
    } catch (const std::exception& e) {
        std::cerr << "Exception 2: " << e.what() << "\n";
        return 1;
    }

    std::cout << "✓ Disk -> RAM Fetch verified.\n";
    std::cout << "Storage layer passed all checks.\n";

    std::remove(test_db.c_str());
    return 0;
}
