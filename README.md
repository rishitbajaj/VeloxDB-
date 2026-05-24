# VeloxDB: High-Performance Custom C++ In-Memory Engine

A handcrafted, ultra-low latency, in-memory key-value store engineered entirely from scratch in C++. This engine completely bypasses standard container wrappers (`std::unordered_map`, `std::vector`, `std::map`) to avoid generic allocation overhead and maximize CPU cache locality. It features a dual-indexing system, a handcrafted recursive parser, and a raw binary length-prefixed serialization framework.

## 🚀 Key Architectural Breakthroughs

* **Zero Standard Container Overhead:** To enforce precise spatial complexity controls and microsecond lookup efficiency, the entire database runs on direct heap pointers (`*`), manual dynamic arrays, and handcrafted linked structures.
* **Dual-Indexing Parity Layer:** Combines a handcrafted Separate-Chaining Hash Map for $O(1)$ point lookups with a customized high-fanout B-Tree architecture to support continuous ordered mapping and rapid structural data access.
* **Custom Lexical Query Parser:** Features an integrated tokenization engine and scanner built explicitly to compile and parse incoming database interaction strings (`INSERT`, `SELECT`, `DELETE`, `SAVE`) dynamically.
* **RAII Dynamic Memory Footprint:** Implements self-contained memory tracking pools (`DynamicTokenArray`) that safely orchestrate real-time growth bounds and heap allocation clean-ups without relying on vector extensions.
* **Length-Prefixed Binary Serialization:** Outperforms slow text-based serialization frameworks (JSON/XML) by streaming memory allocations and data structures into optimized binary disk layouts using length-prefixed serialization primitives.

---

## 🛠️ Engine Design Internals

### 1. The Dual-Index Memory Matrix
The system feeds transaction workloads into two distinct, isolated layout structures to guarantee optimal lookup execution windows:
* **The Points Tier:** A handcrafted hash matrix utilizing separate chaining via nested `HashNode` linked lists. String mapping targets an optimized bitwise shift layout (`hash = ((hash << 5) + hash) + c`) to ensure balanced distribution spreads across $1024$ continuous buckets.
* **The Structural Tree Tier:** A high-fanout B-Tree modeling infrastructure tracking localized child nodes, leaves, and horizontal splits recursively to guarantee $O(\log n)$ balance maintenance thresholds.

### 2. Custom Compilation Pipeline
Instead of consuming input lines through heavy stream buffers, a bespoke scanner maps the characters linearly:
[Raw Console Input] ──► [Lexical Scanner] ──► [Dynamic Token Array] ──► [Engine Switch-Dispatched Commit]

The query parsing block decodes multi-word string statements wrapped inside quotation boundaries natively, mitigating spatial mutations efficiently.

---

## 💻 Technical Specifications & Interface

### Database Grammars Supported
* `INSERT <key> "<value>"` - Commits a variable-length tracking block safely into the storage engine matrix.
* `SELECT <key>` - Queries point targets out of the Hash Tier instantly via raw pointers, outputting high-resolution resolution metrics in nanoseconds.
* `DELETE <key>` - Purges data elements natively across parity dimensions and balances existing trees.
* `SAVE` - Generates a raw binary file snapshot (`velox_store.bin`) reflecting true in-memory layouts.
* `EXIT` - Restores environment states and triggers automated recursive class destructors.

---

## ⚡ Setup, Compiling & Benchmarking

The database codebase is fully self-contained inside a single optimized script file (`veloxdb_core.cpp`) and does not require third-party dependencies.

### Compilation
To maximize loop optimization and trigger vectorization schemes within the low-level custom structures, compile the script using extreme compiler flags (`-O3` aggression tier):

```bash
g++ -O3 veloxdb_core.cpp -o veloxdb_core
```
Execution
Run the compiled binary matrix within any standard terminal window environment:
```
Bash
./veloxdb_core
```
Interactive Usage Sample
```
Plaintext
====================================================================
 🔥 VELOXDB SYSTEM MAX ACTIVE MODE (ZERO STANDARD CONTIGUOUS LIBS)  
 Manual Separate Chaining Hash Map | High-Fanout Custom B-Tree      
 Operations: INSERT key "value" | SELECT key | DELETE key | SAVE     
====================================================================
veloxdb_engine# INSERT candidate101 "Rishit Bajaj - Systems Engineer"
[SYSTEM]: Transaction committed to dual-indexes successfully.

veloxdb_engine# SELECT candidate101
[VALUE]: Rishit Bajaj - Systems Engineer (Retrieved in 45 ns)

veloxdb_engine# SAVE
[PERSISTENCE]: Native binary snapshot flushed cleanly to persistent memory.

veloxdb_engine# EXIT
```
