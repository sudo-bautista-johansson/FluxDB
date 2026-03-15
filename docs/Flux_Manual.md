# FluxDB: Enterprise Game Database Manual

Welcome to FluxDB, the extremely fast, embedded ECS-based database designed specifically for enterprise game engines.

## 1. Introduction
FluxDB is completely embedded, like SQLite, but structured like an **Entity Component System (ECS)**. It completely avoids network roundtrips, living directly inside your game's memory while persisting state durably to disk using 4KB pages and an LRU Buffer Pool.

## 2. Setting Up in Your Engine

### Unity (C#)
1. Drop the `flux.dll` in your `Plugins/` folder.
2. Ensure `FluxDB.cs` is in your scripts folder.
3. Import and use:
```csharp
using fluxdb;

void Start() {
    // Create or open the local embedded database
    FluxDB db = new FluxDB("saves/world_data.fdb");
    
    // Run GQL directly
    db.Query("SPAWN PREFAB 'player' WITH health = 100, tag = 'hero';");
}
```

### Unreal Engine / Custom Engine (C++)
Include FluxDB library via CMake and instantiate the C++ wrapper:
```cpp
#include "flux.h"

int main() {
    fluxdb::Database db("saves/world_data.fdb");
    db.query("SPAWN PREFAB 'player' WITH health = 100, tag = 'hero';");
    return 0;
}
```

## 3. Game Query Language (GQL)
FluxDB uses GQL, an SQL-like dialect heavily optimized for game scenarios like spatial queries and temporal logic.

### Modifying State
```sql
-- Spawning an entity (insert)
SPAWN PREFAB 'goblin' WITH health = 100, tag = 'enemy';

-- Updating multiple entities instantly
UPDATE entities SET health = health - 50 WHERE tag = 'enemy';

-- Cleaning up
DELETE FROM entities WHERE health <= 0;
```

### Spatial Queries (The Core Power)
Finding objects on the map is traditionally slow. FluxDB uses a native Octree to make this query instant ($O(log N)$) even with millions of objects:
```sql
FIND entities 
NEAR (10.0, 0.0, 5.0) 
WITHIN 50.0 
WHERE tag = 'enemy';
```

## 4. Using Flux Studio
Flux Studio is the native Desktop GUI designed for technical designers and developers to debug the `.fdb` files visually without compiling the game.

To launch the Studio on a specific database file:
```powershell
flux.exe serve -port 8080 saves/world_data.fdb
```
The studio features **IntelliSense Autocomplete** for GQL and a fast execution runner. Because FluxDB is embedded, ensure the game is NOT locking the file when opening the Studio.
