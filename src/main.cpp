#include <iostream>
#include <unordered_map>
#include <string>
#include <chrono>
#include <winsock2.h>
#include <windows.h> // Required for CreateThread

#pragma comment(lib, "ws2_32.lib")

struct Record {
    int id;
    std::string name;
    std::chrono::steady_clock::time_point expiry;
};

class VeloxDB {
private:
    std::unordered_map<int, Record> hash_index;
public:
    std::string process_command(std::string raw) {
        auto now = std::chrono::steady_clock::now();
        if (raw.find("INSERT") == 0) {
            try {
                size_t s1 = raw.find(' '), s2 = raw.find(' ', s1 + 1);
                int id = std::stoi(raw.substr(s1 + 1, s2 - s1 - 1));
                std::string name = raw.substr(s2 + 1);
                hash_index[id] = {id, name, now + std::chrono::seconds(30)};
                return "SUCCESS: Inserted " + name;
            } catch (...) { return "ERROR: Invalid Insert"; }
        } 
        if (raw.find("SELECT") == 0) {
            try {
                int id = std::stoi(raw.substr(7));
                if (hash_index.count(id)) {
                    if (now > hash_index[id].expiry) {
                        hash_index.erase(id);
                        return "ERROR: Expired";
                    }
                    return "FOUND: " + hash_index[id].name;
                }
                return "ERROR: Not Found";
            } catch (...) { return "ERROR: Invalid Select"; }
        }
        return "ERROR: Unknown Command";
    }
};

VeloxDB db;

// Windows-specific thread function signature
DWORD WINAPI handle_client(LPVOID lpParam) {
    SOCKET client_socket = (SOCKET)lpParam;
    char buffer[1024] = {0};
    int bytesReceived = recv(client_socket, buffer, 1024, 0);
    
    if (bytesReceived > 0) {
        std::string cmd(buffer);
        std::cout << "[CLIENT] Request: " << cmd << std::endl;
        std::string result = db.process_command(cmd) + "\n";
        send(client_socket, result.c_str(), (int)result.length(), 0);
    }
    
    closesocket(client_socket);
    return 0;
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    
    sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = INADDR_ANY;
    server.sin_port = htons(9999);

    bind(s, (struct sockaddr *)&server, sizeof(server));
    listen(s, 5);
    std::cout << ">>> VELOXDB SERVER RUNNING ON PORT 9999 <<<" << std::endl;

    while (true) {
        SOCKET new_sock = accept(s, NULL, NULL);
        if (new_sock != INVALID_SOCKET) {
            // Using Native Windows Threads to bypass the 'std::thread' error
            CreateThread(NULL, 0, handle_client, (LPVOID)new_sock, 0, NULL);
        }
    }

    WSACleanup();
    return 0;
}