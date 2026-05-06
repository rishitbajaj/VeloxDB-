#include <iostream>
#include <map>
#include <unordered_map>
#include <fstream>
#include <string>

struct Record {
    int id;
    std::string name;
};

class VeloxDB {
private:
    std::unordered_map<int, Record> hash_index; 
    std::map<int, Record> btree_index;         
    const std::string log_path = "data/redo.log";

    void log_to_disk(std::string cmd, int id, std::string name = "") {
        std::ofstream log_file(log_path, std::ios::app);
        if (log_file.is_open()) {
            log_file << cmd << " " << id << " " << name << "\n";
            log_file.close();
        }
    }

public:
    void insert(int id, std::string name, bool is_recovery = false) {
        Record r = {id, name};
        hash_index[id] = r;
        btree_index[id] = r;

        if (!is_recovery) {
            log_to_disk("INSERT", id, name);
            std::cout << "[DB] Successfully inserted " << name << std::endl;
        }
    }

    void select(int id) {
        if (hash_index.count(id)) {
            std::cout << "[SUCCESS] Found ID " << id << ": " << hash_index[id].name << std::endl;
        } else {
            std::cout << "[ERROR] ID " << id << " not found." << std::endl;
        }
    }

    void recover_data() {
        std::ifstream log_file(log_path);
        if (!log_file.is_open()) {
            std::cout << "[SYSTEM] No log file found. Starting fresh." << std::endl;
            return;
        }

        std::string cmd, name;
        int id;
        int count = 0;
        while (log_file >> cmd >> id >> name) {
            if (cmd == "INSERT") {
                insert(id, name, true); 
                count++;
            }
        }
        std::cout << "[SYSTEM] Recovered " << count << " records from data/redo.log" << std::endl;
    }
};

int main() {
    // Force output to show up even if the program crashes
    std::cout << std::unitbuf; 

    VeloxDB db;
    
    std::cout << "--- VeloxDB Engine Starting ---" << std::endl;
    db.recover_data(); 

    db.insert(101, "AlphaUser");
    db.insert(102, "BetaUser");
    
    std::cout << "\nTesting Search:" << std::endl;
    db.select(101);
    db.select(999);

    std::cout << "\n--- System Operations Complete ---" << std::endl;
    
    // This keeps the window open on Windows so you can read the output
    std::cout << "\nPress Enter to exit...";
    std::cin.get(); 

    return 0;
}