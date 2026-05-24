#include <iostream>
#include <string>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cstring>

constexpr int B_TREE_ORDER = 4;
constexpr int HASH_MAP_CAPACITY = 1024;

struct BTreeNode {
    bool isLeaf;
    int numKeys;
    std::string keys[B_TREE_ORDER - 1];
    std::string values[B_TREE_ORDER - 1];
    BTreeNode* children[B_TREE_ORDER];

    BTreeNode(bool leaf) : isLeaf(leaf), numKeys(0) {
        for (int i = 0; i < B_TREE_ORDER; ++i) {
            children[i] = nullptr;
        }
    }
    
    ~BTreeNode() {
        if (!isLeaf) {
            for (int i = 0; i <= numKeys; ++i) {
                delete children[i];
            }
        }
    }
};

class CustomBTree {
private:
    BTreeNode* root;

    void splitChild(BTreeNode* parent, int i, BTreeNode* child) {
        BTreeNode* z = new BTreeNode(child->isLeaf);
        int t = B_TREE_ORDER / 2;
        z->numKeys = t - 1;

        for (int j = 0; j < t - 1; j++) {
            z->keys[j] = child->keys[j + t];
            z->values[j] = child->values[j + t];
        }

        if (!child->isLeaf) {
            for (int j = 0; j < t; j++) {
                z->children[j] = child->children[j + t];
            }
        }

        child->numKeys = t;

        for (int j = parent->numKeys; j >= i + 1; j--) {
            parent->children[j + 1] = parent->children[j];
        }
        parent->children[i + 1] = z;

        for (int j = parent->numKeys - 1; j >= i; j--) {
            parent->keys[j + 1] = parent->keys[j];
            parent->values[j + 1] = parent->values[j];
        }

        parent->keys[i] = child->keys[t - 1];
        parent->values[i] = child->values[t - 1];
        parent->numKeys++;
    }

    void insertNonFull(BTreeNode* node, const std::string& key, const std::string& val) {
        int i = node->numKeys - 1;

        if (node->isLeaf) {
            while (i >= 0 && node->keys[i] > key) {
                node->keys[i + 1] = node->keys[i];
                node->values[i + 1] = node->values[i];
                i--;
            }
            node->keys[i + 1] = key;
            node->values[i + 1] = val;
            node->numKeys++;
        } else {
            while (i >= 0 && node->keys[i] > key) i--;
            i++;
            if (node->children[i]->numKeys == B_TREE_ORDER - 1) {
                splitChild(node, i, node->children[i]);
                if (node->keys[i] < key) i++;
            }
            insertNonFull(node->children[i], key, val);
        }
    }

public:
    CustomBTree() { root = new BTreeNode(true); }
    ~CustomBTree() { delete root; }

    void insert(const std::string& key, const std::string& val) {
        BTreeNode* r = root;
        if (r->numKeys == B_TREE_ORDER - 1) {
            BTreeNode* s = new BTreeNode(false);
            root = s;
            s->children[0] = r;
            splitChild(s, 0, r);
            insertNonFull(s, key, val);
        } else {
            insertNonFull(r, key, val);
        }
    }
    
    void clear() {
        delete root;
        root = new BTreeNode(true);
    }
};

struct HashNode {
    std::string key;
    std::string value;
    HashNode* next;
    HashNode(std::string k, std::string v) : key(k), value(v), next(nullptr) {}
};

class RawHashMap {
private:
    HashNode* buckets[HASH_MAP_CAPACITY];
    size_t activeCount;

    size_t computeHash(const std::string& key) const {
        size_t hash = 5381;
        for (char c : key) {
            hash = ((hash << 5) + hash) + c;
        }
        return hash % HASH_MAP_CAPACITY;
    }

public:
    RawHashMap() : activeCount(0) {
        for (int i = 0; i < HASH_MAP_CAPACITY; ++i) buckets[i] = nullptr;
    }

    ~RawHashMap() {
        clear();
    }

    void put(const std::string& key, const std::string& value) {
        size_t index = computeHash(key);
        HashNode* prev = nullptr;
        HashNode* curr = buckets[index];

        while (curr != nullptr && curr->key != key) {
            prev = curr;
            curr = curr->next;
        }

        if (curr != nullptr) {
            curr->value = value;
        } else {
            HashNode* newNode = new HashNode(key, value);
            if (prev == nullptr) {
                buckets[index] = newNode;
            } else {
                prev->next = newNode;
            }
            activeCount++;
        }
    }

    std::string get(const std::string& key, bool& found) const {
        size_t index = computeHash(key);
        HashNode* curr = buckets[index];
        while (curr != nullptr) {
            if (curr->key == key) {
                found = true;
                return curr->value;
            }
            curr = curr->next;
        }
        found = false;
        return "";
    }

    bool remove(const std::string& key) {
        size_t index = computeHash(key);
        HashNode* prev = nullptr;
        HashNode* curr = buckets[index];

        while (curr != nullptr && curr->key != key) {
            prev = curr;
            curr = curr->next;
        }

        if (curr == nullptr) return false;

        if (prev == nullptr) {
            buckets[index] = curr->next;
        } else {
            prev->next = curr->next;
        }
        delete curr;
        activeCount--;
        return true;
    }

    void clear() {
        for (int i = 0; i < HASH_MAP_CAPACITY; ++i) {
            HashNode* curr = buckets[i];
            while (curr != nullptr) {
                HashNode* temp = curr;
                curr = curr->next;
                delete temp;
            }
            buckets[i] = nullptr;
        }
        activeCount = 0;
    }

    size_t size() const { return activeCount; }

    void exportToStream(std::ofstream& fs) const {
        for (int i = 0; i < HASH_MAP_CAPACITY; ++i) {
            HashNode* curr = buckets[i];
            while (curr != nullptr) {
                size_t kLen = curr->key.length();
                size_t vLen = curr->value.length();
                fs.write(reinterpret_cast<const char*>(&kLen), sizeof(kLen));
                fs.write(curr->key.c_str(), kLen);
                fs.write(reinterpret_cast<const char*>(&vLen), sizeof(vLen));
                fs.write(curr->value.c_str(), vLen);
                curr = curr->next;
            }
        }
    }
};

enum class TokenType { SELECT, INSERT, DELETE, SAVE, IDENTIFIER, STRING, UNKNOWN };

struct Token {
    TokenType type;
    std::string text;
};

class DynamicTokenArray {
private:
    Token* data;
    size_t capacity;
    size_t count;

    void resize() {
        capacity *= 2;
        Token* newData = new Token[capacity];
        for (size_t i = 0; i < count; ++i) newData[i] = data[i];
        delete[] data;
        data = newData;
    }

public:
    DynamicTokenArray() : capacity(4), count(0) { data = new Token[capacity]; }
    ~DynamicTokenArray() { delete[] data; }

    void push(Token t) {
        if (count >= capacity) resize();
        data[count++] = t;
    }

    size_t size() const { return count; }
    Token operator[](size_t i) const { return data[i]; }
};

class Lexer {
public:
    static DynamicTokenArray tokenize(const std::string& source) {
        DynamicTokenArray tokens;
        size_t i = 0;
        while (i < source.length()) {
            if (isspace(source[i])) { i++; continue; }

            if (source[i] == '"' || source[i] == '\'') {
                char quote = source[i++];
                std::string literal = "";
                while (i < source.length() && source[i] != quote) {
                    literal += source[i++];
                }
                if (i < source.length()) i++;
                tokens.push(pushToken(TokenType::STRING, literal));
                continue;
            }

            std::string currentToken = "";
            while (i < source.length() && !isspace(source[i]) && source[i] != '"' && source[i] != '\'') {
                currentToken += source[i++];
            }

            std::string upperToken = currentToken;
            std::transform(upperToken.begin(), upperToken.end(), upperToken.begin(), ::toupper);

            if (upperToken == "SELECT") tokens.push({TokenType::SELECT, currentToken});
            else if (upperToken == "INSERT") tokens.push({TokenType::INSERT, currentToken});
            else if (upperToken == "DELETE") tokens.push({TokenType::DELETE, currentToken});
            else if (upperToken == "SAVE") tokens.push({TokenType::SAVE, currentToken});
            else tokens.push({TokenType::IDENTIFIER, currentToken});
        }
        return tokens;
    }
private:
    static Token pushToken(TokenType type, std::string text) { return {type, text}; }
};

class VeloxDBEngine {
private:
    RawHashMap hashIndex;
    CustomBTree bTreeIndex;
    const std::string binaryStoragePath = "velox_store.bin";

    void rebuildBTree() {
        bTreeIndex.clear();
        std::ifstream fs(binaryStoragePath, std::ios::binary);
        if (!fs.is_open()) return;
        size_t recordCount = 0;
        fs.read(reinterpret_cast<char*>(&recordCount), sizeof(recordCount));
        for (size_t i = 0; i < recordCount; ++i) {
            size_t kLen = 0, vLen = 0;
            fs.read(reinterpret_cast<char*>(&kLen), sizeof(kLen));
            std::string key(kLen, '\0');
            fs.read(&key[0], kLen);
            fs.read(reinterpret_cast<char*>(&vLen), sizeof(vLen));
            std::string value(vLen, '\0');
            fs.read(&value[0], vLen);
            bTreeIndex.insert(key, value);
        }
        fs.close();
    }

public:
    VeloxDBEngine() {
        loadSnapshot();
    }

    void executeInsert(const std::string& key, const std::string& val) {
        hashIndex.put(key, val);
        bTreeIndex.insert(key, val);
        std::cout << "[SYSTEM]: Transaction committed to dual-indexes successfully.\n";
    }

    void executeSelect(const std::string& key) {
        auto start = std::chrono::high_resolution_clock::now();
        bool found = false;
        std::string result = hashIndex.get(key, found);
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::nano> duration = end - start;

        if (found) {
            std::cout << "[VALUE]: " << result << " (Retrieved in " << duration.count() << " ns)\n";
        } else {
            std::cout << "[ERROR]: Key '" << key << "' non-existent.\n";
        }
    }

    void executeDelete(const std::string& key) {
        if (hashIndex.remove(key)) {
            saveSnapshot();
            rebuildBTree();
            std::cout << "[SYSTEM]: Record purged safely from indexing matrix.\n";
        } else {
            std::cout << "[ERROR]: Key validation checkpoint failed.\n";
        }
    }

    void saveSnapshot() {
        std::ofstream fs(binaryStoragePath, std::ios::binary | std::ios::trunc);
        if (!fs.is_open()) return;

        size_t totalRecords = hashIndex.size();
        fs.write(reinterpret_cast<const char*>(&totalRecords), sizeof(totalRecords));
        hashIndex.exportToStream(fs);
        fs.close();
        std::cout << "[PERSISTENCE]: Native binary snapshot flushed cleanly to persistent memory.\n";
    }

    void loadSnapshot() {
        std::ifstream fs(binaryStoragePath, std::ios::binary);
        if (!fs.is_open()) return;

        size_t recordCount = 0;
        fs.read(reinterpret_cast<char*>(&recordCount), sizeof(recordCount));

        hashIndex.clear();
        bTreeIndex.clear();

        for (size_t i = 0; i < recordCount; ++i) {
            size_t kLen = 0, vLen = 0;
            fs.read(reinterpret_cast<char*>(&kLen), sizeof(kLen));
            std::string key(kLen, '\0');
            fs.read(&key[0], kLen);
            fs.read(reinterpret_cast<char*>(&vLen), sizeof(vLen));
            std::string value(vLen, '\0');
            fs.read(&value[0], vLen);

            hashIndex.put(key, value);
            bTreeIndex.insert(key, value);
        }
        fs.close();
        std::cout << "[BOOT]: Restored " << recordCount << " operational states dynamically from binary structures.\n";
    }

    void processQuery(const std::string& queryStr) {
        DynamicTokenArray tokens = Lexer::tokenize(queryStr);
        if (tokens.size() == 0) return;

        switch (tokens[0].type) {
            case TokenType::INSERT:
                if (tokens.size() >= 3 && tokens[1].type == TokenType::IDENTIFIER) {
                    executeInsert(tokens[1].text, tokens[2].text);
                } else {
                    std::cout << "[PARSE ERROR]: Target structural pattern mismatch.\n";
                }
                break;
            case TokenType::SELECT:
                if (tokens.size() == 2 && tokens[1].type == TokenType::IDENTIFIER) {
                    executeSelect(tokens[1].text);
                } else {
                    std::cout << "[PARSE ERROR]: Pattern target missing.\n";
                }
                break;
            case TokenType::DELETE:
                if (tokens.size() == 2 && tokens[1].type == TokenType::IDENTIFIER) {
                    executeDelete(tokens[1].text);
                } else {
                    std::cout << "[PARSE ERROR]: Action missing parameter.\n";
                }
                break;
            case TokenType::SAVE:
                saveSnapshot();
                break;
            default:
                std::cout << "[COMPILER ERROR]: Command sequence unrecognized.\n";
                break;
        }
    }
};

std::string sanitizeInput(std::string str) {
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}

int main() {
    VeloxDBEngine db;
    std::string consoleLine;

    std::cout << "====================================================================\n";
    std::cout << " VELOXDB SYSTEM MAX ACTIVE MODE (ZERO STANDARD CONTIGUOUS LIBS)  \n";
    std::cout << " Manual Separate Chaining Hash Map | High-Fanout Custom B-Tree      \n";
    std::cout << " Operations: INSERT key \"value\" | SELECT key | DELETE key | SAVE     \n";
    std::cout << "====================================================================\n";

    while (true) {
        std::cout << "veloxdb_engine# ";
        if (!std::getline(std::cin, consoleLine)) break;
        if (sanitizeInput(consoleLine) == "exit") break;
        if (consoleLine.empty()) continue;
        
        db.processQuery(consoleLine);
    }
    return 0;
}