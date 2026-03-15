# ⚡ FluxDB Master Manual
### The Ultimate Guide to the World's Fastest Embedded Data Engine

Welcome to the **FluxDB** manual. This document is designed to take you from knowing nothing about databases to mastering the high-performance FluxDB engine.

---

## 🚀 1. The Super Simple Setup (3 Steps)

### Step 1: Install the Engine
Go to your `FluxDB` folder on the desktop and double-click **`install.bat`**. This is like "plugging in" the engine to your computer.

### Step 2: Pick Your Language
FluxDB talks to almost every language. 
- **Python**: Go to `bindings/python` and type `pip install .`
- **Unity**: Copy the `upm` folder into your `Packages` folder.
- **Unreal**: Put the `unreal_plugin` folder into your project's `Plugins` folder.

### Step 3: Run your first Query
Open any of the examples (like `bindings/python/test.py`) and run it!

---

## 🧠 2. Basic Concepts (Explained Simply)

FluxDB is an **ECS (Entity Component System)** database.
- **Entities**: Think of these as "Objects" (a player, a sword, a tree).
- **Components**: These are "Data" (Health, Position, Name).
- **Archetypes**: FluxDB groups together similar things automatically to make them run at light-speed.

---

## 📝 3. Commands You Need to Know

### `CREATE TABLE`
Set up the structure for your data.
```sql
CREATE TABLE players (id INT, hp FLOAT, tag STRING);
```

### `SPAWN`
Create a new object in your world.
```sql
SPAWN PREFAB 'ninja' WITH id=1, hp=100.0, tag='hero';
```

### `SELECT`
Ask questions about your data.
```sql
SELECT * FROM players WHERE hp > 50;
```

---

## 🌟 4. Advanced "Magic" Features

### Spatial Search (FIND NEAR)
Find things based on where they are in the 3D world.
```sql
FIND entities NEAR (0.0, 0.0, 0.0) WITHIN 50.0;
```

### Time Travel (AT TICK)
See what happened in the past!
```sql
SELECT * FROM players AT TICK 1500;
```

### LISTEN (Pub/Sub)
Get notified automatically when things change.
```sql
LISTEN SELECT * FROM entities WHERE hp < 20;
```

---

## 🚑 5. Troubleshooting
- **"flux.dll not found"**: Re-run `install.bat`.
- **"Query Failed"**: Make sure you use dots for decimals (e.g., `100.0` instead of `100`).
- **"Namespace not found"**: Ensure you have imported `Flux` or `fluxdb`.

---
*Happy Coding with FluxDB!*
