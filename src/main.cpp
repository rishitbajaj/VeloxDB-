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
    void remove(int id) {
        if (hash_index.erase(id)) { // Removes from Hash Map
            btree_index.erase(id); // Removes from B-Tree
            log_to_disk("DELETE", id); // Records the deletion on disk
            std::cout << "[DB] Successfully deleted ID " << id << std::endl;
        } else {
            std::cout << "[ERROR] Cannot delete: ID " << id << " not found." << std::endl;
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
        while (log_file >> cmd >> id) {
        if (cmd == "INSERT") {
            log_file >> name;
            insert(id, name, true); 
        } else if (cmd == "DELETE") {
            remove(id); // If the log says DELETE, remove it from RAM
        }
    }
        std::cout << "[SYSTEM] Recovered " << count << " records from data/redo.log" << std::endl;
    }
};

int main() {
    // 1. Initialize the Engine
    VeloxDB db;
    
    std::cout << "--- VeloxDB Engine Starting ---" << std::endl;

    // 2. RECOVERY PHASE
    // This is the most important step. It reads data/redo.log 
    // and rebuilds the memory so you don't lose data from last time.
    db.recover_data(); 

    // 3. CREATE (Insert)
    // We add some users to the database.
    std::cout << "\n[Step 1] Inserting new records..." << std::endl;
    db.insert(101, "AlphaUser");
    db.insert(102, "BetaUser");
    db.insert(103, "GammaUser");

    // 4. READ (Select)
    // We check if the database can find the user we just added.
    std::cout << "\n[Step 2] Testing Search (Point Lookup):" << std::endl;
    db.select(101); // Should succeed
    db.select(999); // Should show "Not Found" error

    // 5. DELETE
    // We remove a user to show the database can clean up memory.
    std::cout << "\n[Step 3] Testing Delete:" << std::endl;
    db.remove(102); // Deletes BetaUser
    db.select(102); // Now this should fail

    // 6. Shutdown
    std::cout << "\n--- System Operations Complete ---" << std::endl;
    
    // Keeps the window open on Windows so you can see the results
    std::cout << "\nPress Enter to close the database...";
    std::cin.ignore(); // Clears any leftover input
    std::cin.get();    // Waits for you to hit Enter

    return 0;
}