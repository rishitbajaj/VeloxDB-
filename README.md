# VeloxDB: High-Performance In-Memory Engine

VeloxDB is a lightweight, high-speed key-value database engine written in C++. It is designed for low-latency applications that require the speed of in-memory processing combined with the reliability of disk-based persistence.

## 🚀 Features
- **Hybrid Indexing:** Uses a Hash Map for $O(1)$ lookups and a B-Tree for sorted range scans.
- **ACID Persistence:** Implements Write-Ahead Logging (WAL) to ensure data is never lost, even if the system crashes.
- **CRUD Operations:** Full support for Creating, Reading, Updating, and Deleting records.
- **Recovery Engine:** Automatically rebuilds the in-memory state from the `redo.log` file on startup.

## 🛠 Project Architecture
The system follows a tiered architecture:
1. **Memory Layer:** Fast RAM-based storage using C++ Standard Template Library.
2. **Persistence Layer:** Append-only logging to ensure durability.
3. **Validation Layer:** Python-based benchmarking suite to measure latency.

## 🚦 Getting Started (Windows)

### 1. Compile the Engine
Use `g++` with high-level optimization:
```powershell
g++ -O3 -std=c++11 src/main.cpp -o veloxdb.exe
