#pragma once

#include "flux_c_api.h"
#include <string>
#include <stdexcept>

namespace fluxdb {

// FluxDB C++ Wrapper
class Database {
public:
    Database();
    ~Database();

    // Prevent copy
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Allow move
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    // Execute a query and return string output
    std::string query(const std::string& sql);

private:
    FluxDB* db_;
};

} // namespace fluxdb
