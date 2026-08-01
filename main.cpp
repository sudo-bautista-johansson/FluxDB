#include <iostream>
#include <sstream>
#include <string_view>
#include "core/headers/planner.h"
#include "core/headers/executor.h"

using namespace fluxdb::query;

int main(int argc, char** argv) {
    std::string sql;
    
    if (argc > 1) {
        sql = argv[1];
    } else {
        // Si no hay argumentos, lee de standard input
        std::ostringstream ss;
        ss << std::cin.rdbuf();
        sql = ss.str();
    }
    
    if (sql.empty()) return 0;
    
    try {
        Planner planner(sql);
        auto plan = planner.plan();
        
        // El executor de FluxDB imprime los resultados por cout
        Executor executor;
        executor.execute(plan);
        std::cout << "\n";
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}