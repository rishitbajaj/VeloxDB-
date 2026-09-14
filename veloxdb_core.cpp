// VeloxDB - high-performance in-memory key-value engine.
//
// Storage is split across two hand-written indexes that are kept in sync:
//   * a separate-chaining hash map for O(1) point lookups
//   * a high-fanout B-Tree for O(log n) ordered access and range scans
//
// Durability is snapshot + write-ahead log: every mutation is appended to the
// log before it is acknowledged, so an interrupted process recovers on restart.

#include <iostream>
#include <iomanip>
#include <fstream>
#include <string>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdint.h>

// Minimum degree t: a node holds up to 2t-1 keys and 2t children. A fanout of
// 64 keeps the tree shallow enough that an entire search path stays hot in L2.
constexpr int B_TREE_MIN_DEGREE = 32;
constexpr int B_TREE_MAX_KEYS = 2 * B_TREE_MIN_DEGREE - 1;
constexpr int B_TREE_MAX_CHILDREN = 2 * B_TREE_MIN_DEGREE;

constexpr size_t HASH_INITIAL_BUCKETS = 1024;
constexpr double HASH_MAX_LOAD_FACTOR = 0.75;

constexpr uint32_t SNAPSHOT_MAGIC = 0x564C5844;  // "VLXD"
constexpr uint8_t WAL_OP_PUT = 1;
constexpr uint8_t WAL_OP_DELETE = 2;

typedef std::chrono::steady_clock BenchClock;

static double elapsedNanos(BenchClock::time_point start, BenchClock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - start).count();
}

// ---------------------------------------------------------------------------
// DynamicArray: the manual growable buffer used instead of std::vector.
// ---------------------------------------------------------------------------

template <typename T>
class DynamicArray {
private:
    T* data;
    size_t capacity;
    size_t count;

    void grow(size_t required) {
        size_t newCapacity = capacity ? capacity : 4;
        while (newCapacity < required) newCapacity *= 2;

        T* newData = new T[newCapacity];
        for (size_t i = 0; i < count; ++i) newData[i] = data[i];
        delete[] data;

        data = newData;
        capacity = newCapacity;
    }

public:
    DynamicArray() : data(new T[4]), capacity(4), count(0) {}

    DynamicArray(const DynamicArray& other)
        : data(new T[other.capacity]), capacity(other.capacity), count(other.count) {
        for (size_t i = 0; i < count; ++i) data[i] = other.data[i];
    }

    DynamicArray& operator=(const DynamicArray& other) {
        if (this == &other) return *this;

        T* newData = new T[other.capacity];
        for (size_t i = 0; i < other.count; ++i) newData[i] = other.data[i];
        delete[] data;

        data = newData;
        capacity = other.capacity;
        count = other.count;
        return *this;
    }

    DynamicArray(DynamicArray&& other) noexcept
        : data(other.data), capacity(other.capacity), count(other.count) {
        other.data = nullptr;
        other.capacity = 0;
        other.count = 0;
    }

    DynamicArray& operator=(DynamicArray&& other) noexcept {
        if (this == &other) return *this;

        delete[] data;
        data = other.data;
        capacity = other.capacity;
        count = other.count;

        other.data = nullptr;
        other.capacity = 0;
        other.count = 0;
        return *this;
    }

    ~DynamicArray() { delete[] data; }

    void reserve(size_t required) {
        if (required > capacity) grow(required);
    }

    void push(const T& value) {
        if (count >= capacity) grow(count + 1);
        data[count++] = value;
    }

    void insertAt(size_t index, const T& value) {
        if (count >= capacity) grow(count + 1);
        for (size_t i = count; i > index; --i) data[i] = data[i - 1];
        data[index] = value;
        count++;
    }

    void clear() { count = 0; }

    size_t size() const { return count; }

    T& operator[](size_t i) { return data[i]; }
    const T& operator[](size_t i) const { return data[i]; }
};

struct Record {
    std::string key;
    std::string value;

    Record() {}
    Record(const std::string& k, const std::string& v) : key(k), value(v) {}
};

// ---------------------------------------------------------------------------
// CustomBTree: ordered index supporting search, range scan, insert and delete.
// ---------------------------------------------------------------------------

struct BTreeNode {
    bool isLeaf;
    int numKeys;
    std::string* keys;
    std::string* values;
    BTreeNode** children;

    explicit BTreeNode(bool leaf)
        : isLeaf(leaf),
          numKeys(0),
          keys(new std::string[B_TREE_MAX_KEYS]),
          values(new std::string[B_TREE_MAX_KEYS]),
          children(new BTreeNode*[B_TREE_MAX_CHILDREN]) {
        for (int i = 0; i < B_TREE_MAX_CHILDREN; ++i) children[i] = nullptr;
    }

    ~BTreeNode() {
        if (!isLeaf) {
            for (int i = 0; i <= numKeys; ++i) delete children[i];
        }
        delete[] keys;
        delete[] values;
        delete[] children;
    }

    // Index of the first key >= target (binary search: nodes hold up to 63 keys).
    int lowerBound(const std::string& target) const {
        int lo = 0;
        int hi = numKeys;
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (keys[mid] < target) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }
};

class CustomBTree {
private:
    BTreeNode* root;
    size_t recordCount;

    // Releases a node whose children now belong to another node.
    static void discardShell(BTreeNode* node) {
        node->isLeaf = true;  // stops the destructor from recursing into moved children
        node->numKeys = 0;
        delete node;
    }

    std::string* searchNode(BTreeNode* node, const std::string& key) const {
        while (node != nullptr) {
            int i = node->lowerBound(key);
            if (i < node->numKeys && node->keys[i] == key) return &node->values[i];
            if (node->isLeaf) return nullptr;
            node = node->children[i];
        }
        return nullptr;
    }

    void splitChild(BTreeNode* parent, int index, BTreeNode* child) {
        const int t = B_TREE_MIN_DEGREE;
        BTreeNode* sibling = new BTreeNode(child->isLeaf);
        sibling->numKeys = t - 1;

        for (int j = 0; j < t - 1; ++j) {
            sibling->keys[j] = child->keys[j + t];
            sibling->values[j] = child->values[j + t];
        }

        if (!child->isLeaf) {
            for (int j = 0; j < t; ++j) {
                sibling->children[j] = child->children[j + t];
                child->children[j + t] = nullptr;
            }
        }

        // keys[t-1] moves up into the parent, so the left child keeps t-1 keys.
        child->numKeys = t - 1;

        for (int j = parent->numKeys; j >= index + 1; --j) {
            parent->children[j + 1] = parent->children[j];
        }
        parent->children[index + 1] = sibling;

        for (int j = parent->numKeys - 1; j >= index; --j) {
            parent->keys[j + 1] = parent->keys[j];
            parent->values[j + 1] = parent->values[j];
        }

        parent->keys[index] = child->keys[t - 1];
        parent->values[index] = child->values[t - 1];
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
            return;
        }

        int child = node->lowerBound(key);
        if (node->children[child]->numKeys == B_TREE_MAX_KEYS) {
            splitChild(node, child, node->children[child]);
            if (node->keys[child] < key) child++;
        }
        insertNonFull(node->children[child], key, val);
    }

    const Record maxRecord(BTreeNode* node) const {
        while (!node->isLeaf) node = node->children[node->numKeys];
        return Record(node->keys[node->numKeys - 1], node->values[node->numKeys - 1]);
    }

    const Record minRecord(BTreeNode* node) const {
        while (!node->isLeaf) node = node->children[0];
        return Record(node->keys[0], node->values[0]);
    }

    void mergeChildren(BTreeNode* parent, int index) {
        const int t = B_TREE_MIN_DEGREE;
        BTreeNode* left = parent->children[index];
        BTreeNode* right = parent->children[index + 1];

        left->keys[t - 1] = parent->keys[index];
        left->values[t - 1] = parent->values[index];

        for (int i = 0; i < right->numKeys; ++i) {
            left->keys[i + t] = right->keys[i];
            left->values[i + t] = right->values[i];
        }

        if (!left->isLeaf) {
            for (int i = 0; i <= right->numKeys; ++i) {
                left->children[i + t] = right->children[i];
                right->children[i] = nullptr;
            }
        }

        for (int i = index + 1; i < parent->numKeys; ++i) {
            parent->keys[i - 1] = parent->keys[i];
            parent->values[i - 1] = parent->values[i];
        }
        for (int i = index + 2; i <= parent->numKeys; ++i) {
            parent->children[i - 1] = parent->children[i];
        }
        parent->children[parent->numKeys] = nullptr;

        left->numKeys += right->numKeys + 1;
        parent->numKeys--;

        discardShell(right);
    }

    void borrowFromPrev(BTreeNode* parent, int index) {
        BTreeNode* child = parent->children[index];
        BTreeNode* sibling = parent->children[index - 1];

        for (int i = child->numKeys - 1; i >= 0; --i) {
            child->keys[i + 1] = child->keys[i];
            child->values[i + 1] = child->values[i];
        }
        if (!child->isLeaf) {
            for (int i = child->numKeys; i >= 0; --i) {
                child->children[i + 1] = child->children[i];
            }
            child->children[0] = sibling->children[sibling->numKeys];
            sibling->children[sibling->numKeys] = nullptr;
        }

        child->keys[0] = parent->keys[index - 1];
        child->values[0] = parent->values[index - 1];
        parent->keys[index - 1] = sibling->keys[sibling->numKeys - 1];
        parent->values[index - 1] = sibling->values[sibling->numKeys - 1];

        child->numKeys++;
        sibling->numKeys--;
    }

    void borrowFromNext(BTreeNode* parent, int index) {
        BTreeNode* child = parent->children[index];
        BTreeNode* sibling = parent->children[index + 1];

        child->keys[child->numKeys] = parent->keys[index];
        child->values[child->numKeys] = parent->values[index];
        if (!child->isLeaf) {
            child->children[child->numKeys + 1] = sibling->children[0];
        }

        parent->keys[index] = sibling->keys[0];
        parent->values[index] = sibling->values[0];

        for (int i = 1; i < sibling->numKeys; ++i) {
            sibling->keys[i - 1] = sibling->keys[i];
            sibling->values[i - 1] = sibling->values[i];
        }
        if (!sibling->isLeaf) {
            for (int i = 1; i <= sibling->numKeys; ++i) {
                sibling->children[i - 1] = sibling->children[i];
            }
            sibling->children[sibling->numKeys] = nullptr;
        }

        child->numKeys++;
        sibling->numKeys--;
    }

    // Guarantees children[index] has at least t keys before descending into it.
    void fillChild(BTreeNode* parent, int index) {
        const int t = B_TREE_MIN_DEGREE;

        if (index != 0 && parent->children[index - 1]->numKeys >= t) {
            borrowFromPrev(parent, index);
        } else if (index != parent->numKeys && parent->children[index + 1]->numKeys >= t) {
            borrowFromNext(parent, index);
        } else if (index != parent->numKeys) {
            mergeChildren(parent, index);
        } else {
            mergeChildren(parent, index - 1);
        }
    }

    bool removeFromNode(BTreeNode* node, const std::string& key) {
        const int t = B_TREE_MIN_DEGREE;
        int index = node->lowerBound(key);

        if (index < node->numKeys && node->keys[index] == key) {
            if (node->isLeaf) {
                for (int i = index + 1; i < node->numKeys; ++i) {
                    node->keys[i - 1] = node->keys[i];
                    node->values[i - 1] = node->values[i];
                }
                node->numKeys--;
                return true;
            }

            if (node->children[index]->numKeys >= t) {
                Record pred = maxRecord(node->children[index]);
                node->keys[index] = pred.key;
                node->values[index] = pred.value;
                return removeFromNode(node->children[index], pred.key);
            }
            if (node->children[index + 1]->numKeys >= t) {
                Record succ = minRecord(node->children[index + 1]);
                node->keys[index] = succ.key;
                node->values[index] = succ.value;
                return removeFromNode(node->children[index + 1], succ.key);
            }

            mergeChildren(node, index);
            return removeFromNode(node->children[index], key);
        }

        if (node->isLeaf) return false;

        bool descendingIntoLast = (index == node->numKeys);
        if (node->children[index]->numKeys < t) {
            fillChild(node, index);
        }

        // A merge at the last position folds the target into the previous child.
        if (descendingIntoLast && index > node->numKeys) {
            return removeFromNode(node->children[index - 1], key);
        }
        return removeFromNode(node->children[index], key);
    }

    void collectRange(BTreeNode* node,
                      const std::string& low,
                      const std::string& high,
                      DynamicArray<Record>& out) const {
        if (node == nullptr) return;

        int i = node->lowerBound(low);
        if (!node->isLeaf) collectRange(node->children[i], low, high, out);

        for (; i < node->numKeys; ++i) {
            if (node->keys[i] > high) return;
            out.push(Record(node->keys[i], node->values[i]));
            if (!node->isLeaf) collectRange(node->children[i + 1], low, high, out);
        }
    }

public:
    CustomBTree() : root(new BTreeNode(true)), recordCount(0) {}
    ~CustomBTree() { delete root; }

    CustomBTree(const CustomBTree&) = delete;
    CustomBTree& operator=(const CustomBTree&) = delete;

    void insert(const std::string& key, const std::string& val) {
        std::string* existing = searchNode(root, key);
        if (existing != nullptr) {
            *existing = val;
            return;
        }

        if (root->numKeys == B_TREE_MAX_KEYS) {
            BTreeNode* newRoot = new BTreeNode(false);
            newRoot->children[0] = root;
            root = newRoot;
            splitChild(newRoot, 0, newRoot->children[0]);
        }
        insertNonFull(root, key, val);
        recordCount++;
    }

    const std::string* search(const std::string& key) const { return searchNode(root, key); }

    bool remove(const std::string& key) {
        if (!removeFromNode(root, key)) return false;

        if (root->numKeys == 0 && !root->isLeaf) {
            BTreeNode* collapsed = root;
            root = root->children[0];
            collapsed->children[0] = nullptr;
            discardShell(collapsed);
        }
        recordCount--;
        return true;
    }

    void rangeScan(const std::string& low, const std::string& high, DynamicArray<Record>& out) const {
        collectRange(root, low, high, out);
    }

    void clear() {
        delete root;
        root = new BTreeNode(true);
        recordCount = 0;
    }

    size_t size() const { return recordCount; }

    int height() const {
        int levels = 1;
        BTreeNode* node = root;
        while (!node->isLeaf) {
            node = node->children[0];
            levels++;
        }
        return levels;
    }
};

// ---------------------------------------------------------------------------
// RawHashMap: separate chaining with djb2 hashing and load-factor driven growth.
// ---------------------------------------------------------------------------

struct HashNode {
    std::string key;
    std::string value;
    HashNode* next;

    HashNode(const std::string& k, const std::string& v) : key(k), value(v), next(nullptr) {}
};

class RawHashMap {
private:
    HashNode** buckets;
    size_t bucketCount;
    size_t activeCount;

    static size_t hashKey(const std::string& key) {
        size_t hash = 5381;
        for (size_t i = 0; i < key.size(); ++i) {
            hash = ((hash << 5) + hash) + static_cast<unsigned char>(key[i]);
        }
        return hash;
    }

    size_t bucketFor(const std::string& key) const { return hashKey(key) & (bucketCount - 1); }

    // Chains degrade to linear scans past the load factor, so double and relink.
    void rehash() {
        size_t newCount = bucketCount * 2;
        HashNode** newBuckets = new HashNode*[newCount];
        for (size_t i = 0; i < newCount; ++i) newBuckets[i] = nullptr;

        for (size_t i = 0; i < bucketCount; ++i) {
            HashNode* curr = buckets[i];
            while (curr != nullptr) {
                HashNode* next = curr->next;
                size_t index = hashKey(curr->key) & (newCount - 1);
                curr->next = newBuckets[index];
                newBuckets[index] = curr;
                curr = next;
            }
        }

        delete[] buckets;
        buckets = newBuckets;
        bucketCount = newCount;
    }

public:
    RawHashMap()
        : buckets(new HashNode*[HASH_INITIAL_BUCKETS]),
          bucketCount(HASH_INITIAL_BUCKETS),
          activeCount(0) {
        for (size_t i = 0; i < bucketCount; ++i) buckets[i] = nullptr;
    }

    ~RawHashMap() {
        clear();
        delete[] buckets;
    }

    RawHashMap(const RawHashMap&) = delete;
    RawHashMap& operator=(const RawHashMap&) = delete;

    void put(const std::string& key, const std::string& value) {
        size_t index = bucketFor(key);
        HashNode* curr = buckets[index];
        while (curr != nullptr) {
            if (curr->key == key) {
                curr->value = value;
                return;
            }
            curr = curr->next;
        }

        HashNode* node = new HashNode(key, value);
        node->next = buckets[index];
        buckets[index] = node;
        activeCount++;

        if (static_cast<double>(activeCount) > HASH_MAX_LOAD_FACTOR * static_cast<double>(bucketCount)) {
            rehash();
        }
    }

    const std::string* get(const std::string& key) const {
        HashNode* curr = buckets[bucketFor(key)];
        while (curr != nullptr) {
            if (curr->key == key) return &curr->value;
            curr = curr->next;
        }
        return nullptr;
    }

    bool remove(const std::string& key) {
        size_t index = bucketFor(key);
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
        for (size_t i = 0; i < bucketCount; ++i) {
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
    size_t buckets_() const { return bucketCount; }

    double loadFactor() const {
        return static_cast<double>(activeCount) / static_cast<double>(bucketCount);
    }
};

// ---------------------------------------------------------------------------
// Array-based baselines. These exist purely so BENCH can quantify what the
// indexes buy over flat storage; the engine itself never uses them.
// ---------------------------------------------------------------------------

class LinearArrayStore {
private:
    DynamicArray<Record> records;

public:
    void put(const std::string& key, const std::string& value) {
        for (size_t i = 0; i < records.size(); ++i) {
            if (records[i].key == key) {
                records[i].value = value;
                return;
            }
        }
        records.push(Record(key, value));
    }

    void append(const std::string& key, const std::string& value) {
        records.push(Record(key, value));
    }

    const std::string* get(const std::string& key) const {
        for (size_t i = 0; i < records.size(); ++i) {
            if (records[i].key == key) return &records[i].value;
        }
        return nullptr;
    }

    void reserve(size_t n) { records.reserve(n); }
    size_t size() const { return records.size(); }
};

class SortedArrayStore {
private:
    DynamicArray<Record> records;

    size_t lowerBound(const std::string& key) const {
        size_t lo = 0;
        size_t hi = records.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (records[mid].key < key) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        return lo;
    }

public:
    void put(const std::string& key, const std::string& value) {
        size_t index = lowerBound(key);
        if (index < records.size() && records[index].key == key) {
            records[index].value = value;
            return;
        }
        records.insertAt(index, Record(key, value));
    }

    const std::string* get(const std::string& key) const {
        size_t index = lowerBound(key);
        if (index < records.size() && records[index].key == key) return &records[index].value;
        return nullptr;
    }

    void reserve(size_t n) { records.reserve(n); }
    size_t size() const { return records.size(); }
};

// ---------------------------------------------------------------------------
// Query front end: lexer plus a dynamic token buffer.
// ---------------------------------------------------------------------------

enum class TokenType { SELECT, INSERT, DELETE, SAVE, RANGE, STATS, IDENTIFIER, STRING, UNKNOWN };

struct Token {
    TokenType type;
    std::string text;

    Token() : type(TokenType::UNKNOWN) {}
    Token(TokenType t, const std::string& s) : type(t), text(s) {}
};

typedef DynamicArray<Token> DynamicTokenArray;

class Lexer {
public:
    static DynamicTokenArray tokenize(const std::string& source) {
        DynamicTokenArray tokens;
        size_t i = 0;

        while (i < source.length()) {
            if (isspace(static_cast<unsigned char>(source[i]))) {
                i++;
                continue;
            }

            if (source[i] == '"' || source[i] == '\'') {
                char quote = source[i++];
                std::string literal;
                while (i < source.length() && source[i] != quote) literal += source[i++];
                if (i < source.length()) i++;
                tokens.push(Token(TokenType::STRING, literal));
                continue;
            }

            std::string word;
            while (i < source.length() && !isspace(static_cast<unsigned char>(source[i])) &&
                   source[i] != '"' && source[i] != '\'') {
                word += source[i++];
            }

            std::string upper = word;
            std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

            if (upper == "SELECT") tokens.push(Token(TokenType::SELECT, word));
            else if (upper == "INSERT") tokens.push(Token(TokenType::INSERT, word));
            else if (upper == "DELETE") tokens.push(Token(TokenType::DELETE, word));
            else if (upper == "SAVE") tokens.push(Token(TokenType::SAVE, word));
            else if (upper == "RANGE") tokens.push(Token(TokenType::RANGE, word));
            else if (upper == "STATS") tokens.push(Token(TokenType::STATS, word));
            else tokens.push(Token(TokenType::IDENTIFIER, word));
        }
        return tokens;
    }
};

// ---------------------------------------------------------------------------
// Durability: length-prefixed binary snapshot plus a write-ahead log.
// ---------------------------------------------------------------------------

static void writeString(std::ostream& os, const std::string& s) {
    uint32_t length = static_cast<uint32_t>(s.size());
    os.write(reinterpret_cast<const char*>(&length), sizeof(length));
    os.write(s.data(), length);
}

static bool readString(std::istream& is, std::string& out) {
    uint32_t length = 0;
    if (!is.read(reinterpret_cast<char*>(&length), sizeof(length))) return false;

    out.assign(length, '\0');
    if (length == 0) return true;
    return static_cast<bool>(is.read(&out[0], length));
}

class WriteAheadLog {
private:
    std::string path;
    std::ofstream stream;

public:
    explicit WriteAheadLog(const std::string& logPath) : path(logPath) {}
    ~WriteAheadLog() { close(); }

    void open() {
        if (!stream.is_open()) {
            stream.open(path.c_str(), std::ios::binary | std::ios::app);
        }
    }

    void close() {
        if (stream.is_open()) stream.close();
    }

    // Flushed before the caller acknowledges the write, so a process crash
    // after the acknowledgement still replays the record on restart.
    void appendPut(const std::string& key, const std::string& value) {
        if (!stream.is_open()) return;
        stream.put(static_cast<char>(WAL_OP_PUT));
        writeString(stream, key);
        writeString(stream, value);
        stream.flush();
    }

    void appendDelete(const std::string& key) {
        if (!stream.is_open()) return;
        stream.put(static_cast<char>(WAL_OP_DELETE));
        writeString(stream, key);
        stream.flush();
    }

    void truncate() {
        close();
        std::ofstream reset(path.c_str(), std::ios::binary | std::ios::trunc);
        reset.close();
        open();
    }

    size_t replay(RawHashMap& hashIndex, CustomBTree& treeIndex) const {
        std::ifstream in(path.c_str(), std::ios::binary);
        if (!in.is_open()) return 0;

        size_t applied = 0;
        while (true) {
            int opcode = in.get();
            if (opcode == EOF) break;

            std::string key;
            if (!readString(in, key)) break;  // torn tail from an interrupted write

            if (opcode == WAL_OP_PUT) {
                std::string value;
                if (!readString(in, value)) break;
                hashIndex.put(key, value);
                treeIndex.insert(key, value);
            } else if (opcode == WAL_OP_DELETE) {
                hashIndex.remove(key);
                treeIndex.remove(key);
            } else {
                break;
            }
            applied++;
        }
        return applied;
    }
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

class VeloxDBEngine {
private:
    RawHashMap hashIndex;
    CustomBTree bTreeIndex;
    std::string snapshotPath;
    std::string walPath;
    WriteAheadLog wal;
    bool verbose;

    void writeSnapshot(const std::string& target) {
        std::string temp = target + ".tmp";
        std::ofstream out(temp.c_str(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;

        // Drained through the B-Tree so the snapshot lands in key order.
        DynamicArray<Record> ordered;
        ordered.reserve(bTreeIndex.size());
        bTreeIndex.rangeScan("", "\xFF", ordered);

        uint32_t magic = SNAPSHOT_MAGIC;
        uint64_t total = static_cast<uint64_t>(ordered.size());
        out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        out.write(reinterpret_cast<const char*>(&total), sizeof(total));

        for (size_t i = 0; i < ordered.size(); ++i) {
            writeString(out, ordered[i].key);
            writeString(out, ordered[i].value);
        }
        out.close();

        std::remove(target.c_str());
        std::rename(temp.c_str(), target.c_str());
    }

    size_t readSnapshot() {
        std::ifstream in(snapshotPath.c_str(), std::ios::binary);
        if (!in.is_open()) return 0;

        uint32_t magic = 0;
        uint64_t total = 0;
        if (!in.read(reinterpret_cast<char*>(&magic), sizeof(magic))) return 0;
        if (magic != SNAPSHOT_MAGIC) return 0;
        if (!in.read(reinterpret_cast<char*>(&total), sizeof(total))) return 0;

        size_t loaded = 0;
        for (uint64_t i = 0; i < total; ++i) {
            std::string key;
            std::string value;
            if (!readString(in, key)) break;
            if (!readString(in, value)) break;
            hashIndex.put(key, value);
            bTreeIndex.insert(key, value);
            loaded++;
        }
        return loaded;
    }

public:
    VeloxDBEngine(const std::string& snapshot, const std::string& log, bool verboseOutput = true)
        : snapshotPath(snapshot), walPath(log), wal(log), verbose(verboseOutput) {
        recover();
        wal.open();
    }

    void recover() {
        hashIndex.clear();
        bTreeIndex.clear();

        size_t fromSnapshot = readSnapshot();
        size_t fromLog = wal.replay(hashIndex, bTreeIndex);

        if (verbose && (fromSnapshot > 0 || fromLog > 0)) {
            std::cout << "[RECOVERY]: Restored " << fromSnapshot << " records from snapshot and replayed "
                      << fromLog << " write-ahead log entries.\n";
        }
    }

    void executeInsert(const std::string& key, const std::string& val) {
        wal.appendPut(key, val);
        hashIndex.put(key, val);
        bTreeIndex.insert(key, val);
        if (verbose) std::cout << "[SYSTEM]: Committed to hash and B-Tree indexes, logged for recovery.\n";
    }

    void executeSelect(const std::string& key) {
        BenchClock::time_point start = BenchClock::now();
        const std::string* result = hashIndex.get(key);
        BenchClock::time_point end = BenchClock::now();

        if (result == nullptr) {
            std::cout << "[ERROR]: Key '" << key << "' not found.\n";
            return;
        }

        // A single lookup can finish inside one clock tick on platforms with a
        // coarse steady_clock; --bench amortises the probe loop to measure it.
        double ns = elapsedNanos(start, end);
        std::cout << "[VALUE]: " << *result << " (hash index, ";
        if (ns > 0.0) {
            std::cout << std::fixed << std::setprecision(0) << ns << " ns)\n";
        } else {
            std::cout << "under clock resolution; run --bench for calibrated latency)\n";
        }
    }

    void executeDelete(const std::string& key) {
        if (hashIndex.get(key) == nullptr) {
            std::cout << "[ERROR]: Key '" << key << "' not found.\n";
            return;
        }
        wal.appendDelete(key);
        hashIndex.remove(key);
        bTreeIndex.remove(key);
        std::cout << "[SYSTEM]: Record purged from both indexes.\n";
    }

    void executeRange(const std::string& low, const std::string& high) {
        DynamicArray<Record> results;
        bTreeIndex.rangeScan(low, high, results);

        std::cout << "[RANGE]: " << results.size() << " record(s) in [" << low << ", " << high << "]\n";
        for (size_t i = 0; i < results.size(); ++i) {
            std::cout << "  " << results[i].key << " => " << results[i].value << "\n";
        }
    }

    void executeSave() {
        writeSnapshot(snapshotPath);
        wal.truncate();
        std::cout << "[PERSISTENCE]: Binary snapshot written to " << snapshotPath
                  << " and write-ahead log compacted.\n";
    }

    void executeStats() const {
        std::cout << "[STATS]: records=" << hashIndex.size()
                  << " hash_buckets=" << hashIndex.buckets_()
                  << " hash_load_factor=" << std::fixed << std::setprecision(3) << hashIndex.loadFactor()
                  << " btree_records=" << bTreeIndex.size()
                  << " btree_height=" << bTreeIndex.height() << "\n";
    }

    void processQuery(const std::string& queryStr) {
        DynamicTokenArray tokens = Lexer::tokenize(queryStr);
        if (tokens.size() == 0) return;

        switch (tokens[0].type) {
            case TokenType::INSERT:
                if (tokens.size() >= 3 &&
                    tokens[1].type == TokenType::IDENTIFIER &&
                    (tokens[2].type == TokenType::STRING || tokens[2].type == TokenType::IDENTIFIER)) {
                    executeInsert(tokens[1].text, tokens[2].text);
                } else {
                    std::cout << "[PARSE ERROR]: Expected INSERT <key> \"<value>\".\n";
                }
                break;

            case TokenType::SELECT:
                if (tokens.size() == 2 && tokens[1].type == TokenType::IDENTIFIER) {
                    executeSelect(tokens[1].text);
                } else {
                    std::cout << "[PARSE ERROR]: Expected SELECT <key>.\n";
                }
                break;

            case TokenType::DELETE:
                if (tokens.size() == 2 && tokens[1].type == TokenType::IDENTIFIER) {
                    executeDelete(tokens[1].text);
                } else {
                    std::cout << "[PARSE ERROR]: Expected DELETE <key>.\n";
                }
                break;

            case TokenType::RANGE:
                if (tokens.size() == 3) {
                    executeRange(tokens[1].text, tokens[2].text);
                } else {
                    std::cout << "[PARSE ERROR]: Expected RANGE <start_key> <end_key>.\n";
                }
                break;

            case TokenType::SAVE:
                executeSave();
                break;

            case TokenType::STATS:
                executeStats();
                break;

            default:
                std::cout << "[PARSE ERROR]: Unrecognised command '" << tokens[0].text << "'.\n";
                break;
        }
    }

    // Insert-then-lookup workload used for the head-to-head against MySQL.
    void runWorkload(size_t records) {
        std::cout << "[WORKLOAD] records=" << records << "\n";

        BenchClock::time_point start = BenchClock::now();
        for (size_t i = 0; i < records; ++i) {
            std::string key = "user_id_" + std::to_string(i);
            executeInsert(key, "Payload_Record_Data_Block_" + std::to_string(i));
        }
        double insertNs = elapsedNanos(start, BenchClock::now());

        size_t hits = 0;
        start = BenchClock::now();
        for (size_t i = 0; i < records; ++i) {
            // Strided probe order to defeat sequential prefetching.
            size_t index = (i * 7919) % records;
            if (hashIndex.get("user_id_" + std::to_string(index)) != nullptr) hits++;
        }
        double selectNs = elapsedNanos(start, BenchClock::now());

        std::cout << std::fixed << std::setprecision(3);
        std::cout << "[WORKLOAD] insert_total_s=" << insertNs / 1e9 << "\n";
        std::cout << "[WORKLOAD] insert_avg_us=" << (insertNs / records) / 1e3 << "\n";
        std::cout << "[WORKLOAD] insert_ops_per_sec=" << (records * 1e9) / insertNs << "\n";
        std::cout << "[WORKLOAD] select_total_s=" << selectNs / 1e9 << "\n";
        std::cout << "[WORKLOAD] select_avg_us=" << (selectNs / records) / 1e3 << "\n";
        std::cout << "[WORKLOAD] select_ops_per_sec=" << (records * 1e9) / selectNs << "\n";
        std::cout << "[WORKLOAD] select_hits=" << hits << "\n";
    }

    void setVerbose(bool value) { verbose = value; }
};

// ---------------------------------------------------------------------------
// Index micro-benchmark: indexed lookups vs flat array storage.
// ---------------------------------------------------------------------------

namespace {

volatile size_t benchSink = 0;

// Repeats the probe loop until enough time has accumulated to be measurable on
// a coarse system clock, then reports nanoseconds per single lookup.
template <typename LookupFn>
double measureLookupNs(LookupFn lookup, size_t probes, double minTotalMs) {
    size_t rounds = 0;
    size_t found = 0;
    BenchClock::time_point start = BenchClock::now();
    double elapsed = 0.0;

    do {
        for (size_t i = 0; i < probes; ++i) {
            if (lookup(i)) found++;
        }
        rounds++;
        elapsed = elapsedNanos(start, BenchClock::now());
    } while (elapsed < minTotalMs * 1e6 && rounds < 10000);

    benchSink += found;
    return elapsed / static_cast<double>(probes * rounds);
}

std::string benchKey(size_t i) { return "user_id_" + std::to_string(i); }

}  // namespace

static void runIndexBenchmark(size_t records) {
    // Linear scan is O(n) per probe, so cap the probe set to keep the flat
    // baseline tractable at high record counts. Every structure sees the same
    // probe sequence.
    size_t probes = records < 2000 ? records : 2000;

    RawHashMap hashIndex;
    CustomBTree treeIndex;
    LinearArrayStore linearStore;
    SortedArrayStore sortedStore;
    linearStore.reserve(records);
    sortedStore.reserve(records);

    for (size_t i = 0; i < records; ++i) {
        std::string key = benchKey(i);
        std::string value = "Payload_Record_Data_Block_" + std::to_string(i);
        hashIndex.put(key, value);
        treeIndex.insert(key, value);
        linearStore.append(key, value);
        sortedStore.put(key, value);
    }

    DynamicArray<std::string> probeKeys;
    probeKeys.reserve(probes);
    for (size_t i = 0; i < probes; ++i) {
        probeKeys.push(benchKey((i * 7919) % records));
    }

    const RawHashMap& hashRef = hashIndex;
    const CustomBTree& treeRef = treeIndex;
    const LinearArrayStore& linearRef = linearStore;
    const SortedArrayStore& sortedRef = sortedStore;

    double hashNs = measureLookupNs(
        [&](size_t i) { return hashRef.get(probeKeys[i]) != nullptr; }, probes, 200.0);
    double treeNs = measureLookupNs(
        [&](size_t i) { return treeRef.search(probeKeys[i]) != nullptr; }, probes, 200.0);
    double sortedNs = measureLookupNs(
        [&](size_t i) { return sortedRef.get(probeKeys[i]) != nullptr; }, probes, 200.0);
    double linearNs = measureLookupNs(
        [&](size_t i) { return linearRef.get(probeKeys[i]) != nullptr; }, probes, 200.0);

    double indexedNs = hashNs < treeNs ? hashNs : treeNs;

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "[BENCH] records=" << records << "\n";
    std::cout << "[BENCH] probes=" << probes << "\n";
    std::cout << "[BENCH] hash_lookup_ns=" << hashNs << "\n";
    std::cout << "[BENCH] btree_lookup_ns=" << treeNs << "\n";
    std::cout << "[BENCH] sorted_array_lookup_ns=" << sortedNs << "\n";
    std::cout << "[BENCH] linear_array_lookup_ns=" << linearNs << "\n";
    std::cout << "[BENCH] btree_height=" << treeIndex.height() << "\n";
    std::cout << "[BENCH] hash_load_factor=" << hashIndex.loadFactor() << "\n";
    // "Faster by" as a percentage reduction in latency, capped at 100%.
    std::cout << "[BENCH] faster_than_linear_pct=" << (1.0 - indexedNs / linearNs) * 100.0 << "\n";
    std::cout << "[BENCH] faster_than_sorted_pct=" << (1.0 - indexedNs / sortedNs) * 100.0 << "\n";
    std::cout << "[BENCH] speedup_vs_linear_x=" << linearNs / indexedNs << "\n";
    std::cout << "[BENCH] speedup_vs_sorted_x=" << sortedNs / indexedNs << "\n";
}

// ---------------------------------------------------------------------------

static void printBanner() {
    std::cout << "====================================================================\n";
    std::cout << " VELOXDB - IN-MEMORY KEY-VALUE ENGINE\n";
    std::cout << " Separate-chaining hash index | High-fanout B-Tree | WAL recovery\n";
    std::cout << " INSERT key \"value\" | SELECT key | DELETE key | RANGE lo hi | SAVE | STATS | EXIT\n";
    std::cout << "====================================================================\n";
}

int main(int argc, char** argv) {
    std::string snapshotPath = "velox_store.bin";
    std::string walPath = "velox_store.wal";
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--bench" && i + 1 < argc) {
            runIndexBenchmark(static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10)));
            return 0;
        }
        if (arg == "--workload" && i + 1 < argc) {
            size_t records = static_cast<size_t>(std::strtoul(argv[++i], nullptr, 10));
            std::string benchSnapshot = "velox_bench.bin";
            std::string benchWal = "velox_bench.wal";
            std::remove(benchSnapshot.c_str());
            std::remove(benchWal.c_str());
            {
                VeloxDBEngine bench(benchSnapshot, benchWal, false);
                bench.runWorkload(records);
            }
            std::remove(benchSnapshot.c_str());
            std::remove(benchWal.c_str());
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
            continue;
        }
        if (arg == "--help") {
            std::cout << "Usage: veloxdb_core [--bench N] [--workload N] [--quiet]\n";
            return 0;
        }
    }

    VeloxDBEngine db(snapshotPath, walPath, !quiet);
    if (!quiet) printBanner();

    std::string line;
    while (true) {
        if (!quiet) std::cout << "veloxdb_engine# ";
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::string lowered = line;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), ::tolower);
        if (lowered == "exit" || lowered == "quit") break;

        db.processQuery(line);
    }
    return 0;
}
