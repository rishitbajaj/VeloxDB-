# VeloxDB

A high-concurrency, multi-threaded In-Memory Key-Value store built in C++ with a Python testing suite.

## 🚀 Features
* **Multi-threaded Architecture:** Uses Native Windows Threading (Win32 API) to handle multiple simultaneous client connections.
* **TCP/IP Networking:** Custom protocol layer allowing remote data interaction via Port 9999.
* **Time-To-Live (TTL):** Automatic data expiration logic using `std::chrono` to manage memory volatility.
* **Optimized Indexing:** Leveraging `std::unordered_map` for $O(1)$ average-time complexity on data retrieval.

## 🛠️ Tech Stack
* **Server:** C++ (WinSock2, Windows API)
* **Client/Testing:** Python 3 (Socket, Threading)
* **Compiler:** MinGW / G++

## 🚦 Getting Started

### 1. Compile the Server
```powershell
g++ -O3 src/main.cpp -o veloxdb.exe -lws2_32
