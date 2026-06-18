// Lab 6 - In-memory transaction manager (MVCC + Strict 2PL + deadlock detection)
// MVCC handles reads via per-key version chains and a snapshot taken on start();
// Strict 2PL handles writes via shared/exclusive row locks held until commit;
// a DFS over the waits-for graph detects deadlocks and aborts the youngest tx.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

using TxnId = uint64_t;
using Stamp = uint64_t;
using RowKey = string;

enum class TxnState { Running, Finished, Killed };
enum class LockKind { Read, Write };

struct TxnFailure : runtime_error {
    explicit TxnFailure(const string& why) : runtime_error(why) {}
};

struct TxnRecord {
    TxnId    tid;
    Stamp    snap;
    Stamp    commitStamp = 0;
    TxnState state       = TxnState::Running;
    bool     shrinking   = false;
};

struct RowVersion {
    string data;
    TxnId  creator;
    TxnId  invalidator;
    bool   deleted;
};

struct LockOwner {
    TxnId    owner;
    LockKind kind;
};

class TxManager {
public:
    TxnId start() {
        lock_guard<mutex> lk(txnMu);
        TxnId id = nextId++;
        txnTable[id] = TxnRecord{id, globalStamp.load(), 0, TxnState::Running, false};
        return id;
    }

    optional<string> fetch(TxnId tx, const RowKey& k) {
        lockRow(tx, k, LockKind::Read);
        lock_guard<mutex> lk(storeMu);
        auto it = store.find(k);
        if (it == store.end()) return nullopt;
        for (const RowVersion& v : it->second) {
            if (!isVisible(v, tx)) continue;
            if (v.deleted) return nullopt;
            return v.data;
        }
        return nullopt;
    }

    void store_(TxnId tx, const RowKey& k, const string& data) {
        lockRow(tx, k, LockKind::Write);
        lock_guard<mutex> lk(storeMu);
        auto& chain = store[k];

        ensureWritable(tx, chain);

        for (RowVersion& v : chain) {
            if (isVisible(v, tx) && v.invalidator == 0) {
                v.invalidator = tx;
                break;
            }
        }
        chain.push_front({data, tx, 0, false});
    }

    void erase_(TxnId tx, const RowKey& k) {
        lockRow(tx, k, LockKind::Write);
        lock_guard<mutex> lk(storeMu);
        auto it = store.find(k);
        if (it == store.end()) return;

        ensureWritable(tx, it->second);

        for (RowVersion& v : it->second) {
            if (isVisible(v, tx) && v.invalidator == 0) {
                v.invalidator = tx;
                it->second.push_front({"", tx, 0, true});
                return;
            }
        }
    }

    void commitTxn(TxnId tx) {
        {
            lock_guard<mutex> lk(txnMu);
            txnTable[tx].state       = TxnState::Finished;
            txnTable[tx].commitStamp = ++globalStamp;
            txnTable[tx].shrinking   = true;
        }
        releaseAll(tx);
    }

    void abortTxn(TxnId tx) {
        {
            lock_guard<mutex> lk(txnMu);
            txnTable[tx].state     = TxnState::Killed;
            txnTable[tx].shrinking = true;
        }
        releaseAll(tx);
    }

    size_t vacuum() {
        Stamp horizon;
        {
            lock_guard<mutex> lk(txnMu);
            horizon = globalStamp.load();
            for (auto& [_, t] : txnTable) {
                if (t.state == TxnState::Running && t.snap < horizon)
                    horizon = t.snap;
            }
        }
        size_t pruned = 0;
        lock_guard<mutex> lk(storeMu);
        for (auto& [_, chain] : store) {
            for (auto it = chain.begin(); it != chain.end();) {
                bool dead = it->invalidator != 0
                            && finishedBefore(it->invalidator, horizon);
                if (dead) { it = chain.erase(it); ++pruned; }
                else      { ++it; }
            }
        }
        return pruned;
    }

    size_t chainLength(const RowKey& k) {
        lock_guard<mutex> lk(storeMu);
        auto it = store.find(k);
        return it == store.end() ? 0 : it->second.size();
    }

private:
    bool finishedBefore(TxnId tx, Stamp ts) {
        lock_guard<mutex> lk(txnMu);
        auto it = txnTable.find(tx);
        if (it == txnTable.end() || it->second.state != TxnState::Finished) return false;
        return it->second.commitStamp <= ts;
    }

    bool isFinished(TxnId tx) {
        lock_guard<mutex> lk(txnMu);
        auto it = txnTable.find(tx);
        return it != txnTable.end() && it->second.state == TxnState::Finished;
    }

    Stamp commitStampOf(TxnId tx) {
        lock_guard<mutex> lk(txnMu);
        auto it = txnTable.find(tx);
        return it == txnTable.end() ? 0 : it->second.commitStamp;
    }

    bool isVisible(const RowVersion& v, TxnId reader) {
        Stamp snap;
        {
            lock_guard<mutex> lk(txnMu);
            snap = txnTable.at(reader).snap;
        }
        bool creatorVisible = (v.creator == reader) || finishedBefore(v.creator, snap);
        if (!creatorVisible) return false;
        if (v.invalidator == 0) return true;
        bool invalidatorVisible = (v.invalidator == reader) || finishedBefore(v.invalidator, snap);
        return !invalidatorVisible;
    }

    void ensureWritable(TxnId tx, const list<RowVersion>& chain) {
        Stamp snap;
        {
            lock_guard<mutex> lk(txnMu);
            snap = txnTable.at(tx).snap;
        }
        for (const RowVersion& v : chain) {
            if (v.creator == tx) continue;
            if (isFinished(v.creator) && commitStampOf(v.creator) > snap)
                throw TxnFailure("could not serialize access: row touched by tx " + to_string(v.creator));
            if (v.invalidator != 0 && v.invalidator != tx
                && isFinished(v.invalidator) && commitStampOf(v.invalidator) > snap)
                throw TxnFailure("could not serialize access: row touched by tx " + to_string(v.invalidator));
        }
    }

    void lockRow(TxnId tx, const RowKey& k, LockKind kind) {
        unique_lock<mutex> lk(lockMu);

        while (true) {
            {
                lock_guard<mutex> tlk(txnMu);
                if (txnTable[tx].state == TxnState::Killed)
                    throw TxnFailure("aborted by deadlock detector");
                if (txnTable[tx].shrinking)
                    throw TxnFailure("2PL violation: acquire in shrinking phase");
            }
            auto& owners = lockTable[k];

            bool holdRead    = false;
            bool holdWrite   = false;
            bool conflict    = false;
            for (LockOwner& h : owners) {
                if (h.owner == tx) {
                    if (h.kind == LockKind::Write) holdWrite = true;
                    else                           holdRead  = true;
                    continue;
                }
                if (kind == LockKind::Write || h.kind == LockKind::Write)
                    conflict = true;
            }

            if (holdWrite)                               return;
            if (holdRead && kind == LockKind::Read)        return;

            if (holdRead && kind == LockKind::Write && owners.size() == 1) {
                owners.front().kind = LockKind::Write;
                return;
            }

            if (!conflict && !holdRead) {
                owners.push_back({tx, kind});
                return;
            }

            for (LockOwner& h : owners)
                if (h.owner != tx) waitGraph[tx].insert(h.owner);

            if (TxnId victim = detectDeadlockVictim(tx); victim != 0) {
                waitGraph.erase(tx);
                if (victim == tx) throw TxnFailure("deadlock: victim " + to_string(tx));
                {
                    lock_guard<mutex> tlk(txnMu);
                    txnTable[victim].state     = TxnState::Killed;
                    txnTable[victim].shrinking = true;
                }
                dropLocks(victim);
                lockCv.notify_all();
                continue;
            }

            lockCv.wait(lk);
            waitGraph.erase(tx);
        }
    }

    void releaseAll(TxnId tx) {
        {
            unique_lock<mutex> lk(lockMu);
            dropLocks(tx);
            waitGraph.erase(tx);
            for (auto& [_, deps] : waitGraph) deps.erase(tx);
        }
        lockCv.notify_all();
    }

    void dropLocks(TxnId tx) {
        for (auto it = lockTable.begin(); it != lockTable.end();) {
            auto& v = it->second;
            v.erase(remove_if(v.begin(), v.end(),
                             [&](const LockOwner& h){ return h.owner == tx; }),
                    v.end());
            if (v.empty()) it = lockTable.erase(it);
            else           ++it;
        }
    }

    TxnId detectDeadlockVictim(TxnId start) {
        unordered_set<TxnId> onStack, done;
        vector<TxnId> path;

        function<bool(TxnId)> dfs = [&](TxnId u) -> bool {
            onStack.insert(u);
            path.push_back(u);
            for (TxnId v : waitGraph[u]) {
                if (onStack.count(v)) { path.push_back(v); return true; }
                if (!done.count(v) && dfs(v)) return true;
            }
            onStack.erase(u);
            path.back();
            path.pop_back();
            done.insert(u);
            return false;
        };

        if (!dfs(start)) return 0;
        TxnId victim = 0;
        for (TxnId t : path) if (t > victim) victim = t;
        return victim;
    }

    atomic<TxnId>                               nextId{1};
    atomic<Stamp>                               globalStamp{0};

    mutex                                       txnMu;
    unordered_map<TxnId, TxnRecord>             txnTable;

    mutex                                       storeMu;
    unordered_map<RowKey, list<RowVersion>>     store;

    mutex                                       lockMu;
    condition_variable                          lockCv;
    unordered_map<RowKey, vector<LockOwner>>    lockTable;
    unordered_map<TxnId, unordered_set<TxnId>>  waitGraph;
};

static bool quietMode = false;
static mutex ioMu;
static void emit(const string& s) {
    if (quietMode) return;
    lock_guard<mutex> lk(ioMu);
    cout << s << "\n";
}

static void demoSnapshot(TxManager& engine) {
    emit("=== 1. snapshot isolation: reader sees pre-write value ===");
    TxnId seed = engine.start();
    engine.store_(seed, "acct", "1000");
    engine.commitTxn(seed);

    TxnId reader = engine.start();
    TxnId writer = engine.start();
    engine.store_(writer, "acct", "2000");
    engine.commitTxn(writer);

    auto v = engine.fetch(reader, "acct");
    emit("  reader (tx " + to_string(reader) + ") sees: " + v.value_or("<none>"));
    engine.commitTxn(reader);

    if (!v.has_value() || v.value() != "1000") {
        throw runtime_error("Snapshot isolation failed: reader should have seen '1000', but saw '" + v.value_or("<none>") + "'");
    }
}

static void demoSharedLocks(TxManager& engine) {
    emit("=== 2. two readers hold shared locks at the same time ===");
    TxnId a = engine.start();
    TxnId b = engine.start();
    auto va = engine.fetch(a, "acct");
    auto vb = engine.fetch(b, "acct");
    emit("  tx " + to_string(a) + " read: " + va.value_or("<none>"));
    emit("  tx " + to_string(b) + " read: " + vb.value_or("<none>"));
    engine.commitTxn(a);
    engine.commitTxn(b);

    if (!va.has_value() || va.value() != "2000") {
        throw runtime_error("Shared lock read failed for tx " + to_string(a) + ": expected '2000', got '" + va.value_or("<none>") + "'");
    }
    if (!vb.has_value() || vb.value() != "2000") {
        throw runtime_error("Shared lock read failed for tx " + to_string(b) + ": expected '2000', got '" + vb.value_or("<none>") + "'");
    }
}

static void demoBlocking(TxManager& engine) {
    emit("=== 3. exclusive lock blocks a reader, but reader stays at SI snapshot ===");
    TxnId writer = engine.start();
    engine.store_(writer, "acct", "3000");

    optional<string> readerVal = nullopt;
    atomic<bool> readerStarted{false};

    thread readerThread([&] {
        TxnId r = engine.start();
        readerStarted = true;
        emit("  reader (tx " + to_string(r) + ") waiting for shared lock...");
        readerVal = engine.fetch(r, "acct");
        emit("  reader (tx " + to_string(r) + ") got: " + readerVal.value_or("<none>"));
        engine.commitTxn(r);
    });

    // Defensive loop instead of blind reliance on thread scheduling times
    while (!readerStarted.load()) {
        this_thread::yield();
    }
    this_thread::sleep_for(chrono::milliseconds(50));

    engine.commitTxn(writer);
    readerThread.join();

    if (!readerVal.has_value() || readerVal.value() != "2000") {
        throw runtime_error("Blocking test failed: reader should have gotten '2000' from its snapshot, got '" + readerVal.value_or("<none>") + "'");
    }
}

static void demoUpgrade(TxManager& engine) {
    emit("=== 4. lock upgrade S -> X by sole holder ===");
    TxnId t = engine.start();
    auto v = engine.fetch(t, "acct");
    emit("  tx " + to_string(t) + " read under S lock: " + v.value_or("<none>"));
    engine.store_(t, "acct", "4000");
    emit("  tx " + to_string(t) + " upgraded to X lock and wrote 4000");
    engine.commitTxn(t);

    if (!v.has_value() || v.value() != "3000") {
        throw runtime_error("Lock upgrade read failed: expected '3000', got '" + v.value_or("<none>") + "'");
    }

    TxnId verifier = engine.start();
    auto v2 = engine.fetch(verifier, "acct");
    engine.commitTxn(verifier);
    if (!v2.has_value() || v2.value() != "4000") {
        throw runtime_error("Lock upgrade verification failed: expected '4000' after commit, got '" + v2.value_or("<none>") + "'");
    }
}

static void demoDeadlock(TxManager& engine) {
    emit("=== 5. deadlock detection (younger tx aborts) ===");
    TxnId t1 = engine.start();
    TxnId t2 = engine.start();
    engine.store_(t1, "X", "1");
    engine.store_(t2, "Y", "1");

    atomic<int> aborts{0};
    atomic<bool> t1_aborted{false};
    atomic<bool> t2_aborted{false};
    auto run = [&](TxnId tx, const RowKey& other, atomic<bool>& aborted_flag) {
        try {
            engine.store_(tx, other, "2");
            engine.commitTxn(tx);
            emit("  tx " + to_string(tx) + " committed");
        } catch (const TxnFailure& e) {
            engine.abortTxn(tx);
            aborts++;
            aborted_flag = true;
            emit("  tx " + to_string(tx) + " aborted: " + e.what());
        }
    };
    thread th1(run, t1, "Y", ref(t1_aborted));
    thread th2(run, t2, "X", ref(t2_aborted));
    th1.join();
    th2.join();
    if (aborts.load() == 0) {
        emit("  (deadlock detector missed the cycle)");
        throw runtime_error("Deadlock detection failed: no transaction aborted");
    }
    if (!t2_aborted.load()) {
        throw runtime_error("Deadlock detection failed: expected younger transaction (tx " + to_string(t2) + ") to abort");
    }
    if (t1_aborted.load()) {
        throw runtime_error("Deadlock detection failed: older transaction (tx " + to_string(t1) + ") aborted unexpectedly");
    }
}

static void demoLostUpdate(TxManager& engine) {
    emit("=== 6. SI rejects a lost update (first-updater-wins) ===");
    TxnId seed = engine.start();
    engine.store_(seed, "tally", "0");
    engine.commitTxn(seed);

    TxnId a = engine.start();
    TxnId b = engine.start();

    atomic<bool> a_committed{false};
    atomic<bool> b_aborted{false};
    atomic<bool> a_started{false};

    thread th([&] {
        try {
            a_started = true;
            engine.fetch(a, "tally");
            engine.store_(a, "tally", "1");
            engine.commitTxn(a);
            a_committed = true;
            emit("  tx " + to_string(a) + " committed counter=1");
        } catch (const TxnFailure& e) {
            engine.abortTxn(a);
            emit("  tx " + to_string(a) + " aborted: " + e.what());
        }
    });

    while (!a_started.load()) {
        this_thread::yield();
    }
    this_thread::sleep_for(chrono::milliseconds(30));

    try {
        engine.fetch(b, "tally");
        engine.store_(b, "tally", "2");
        engine.commitTxn(b);
        emit("  tx " + to_string(b) + " committed counter=2");
    } catch (const TxnFailure& e) {
        engine.abortTxn(b);
        b_aborted = true;
        emit("  tx " + to_string(b) + " aborted: " + e.what());
    }
    th.join();

    if (!a_committed.load()) {
        throw runtime_error("Lost update prevention failed: tx " + to_string(a) + " failed to commit");
    }
    if (!b_aborted.load()) {
        throw runtime_error("Lost update prevention failed: tx " + to_string(b) + " did not abort on serialization conflict");
    }
}

static void demoVacuum(TxManager& engine) {
    emit("=== 7. vacuum prunes dead versions ===");
    for (int i = 0; i < 5; i++) {
        TxnId t = engine.start();
        engine.store_(t, "gckey", "v" + to_string(i));
        engine.commitTxn(t);
    }
    size_t len_before = engine.chainLength("gckey");
    emit("  vkey chain length before vacuum: " + to_string(len_before));
    size_t pruned = engine.vacuum();
    emit("  vacuum pruned " + to_string(pruned) + " dead versions (across all keys)");
    size_t len_after = engine.chainLength("gckey");
    emit("  vkey chain length after vacuum:  " + to_string(len_after));

    if (len_before != 5) {
        throw runtime_error("Vacuum test failed: chain length before vacuum should be 5, got " + to_string(len_before));
    }
    if (pruned != 8) {
        throw runtime_error("Vacuum test failed: expected 8 pruned versions, got " + to_string(pruned));
    }
    if (len_after != 1) {
        throw runtime_error("Vacuum test failed: chain length after vacuum should be 1, got " + to_string(len_after));
    }
}

int main(int argc, char* argv[]) {
    bool runTests = false;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if (arg == "--test" || arg == "-t") {
            runTests = true;
        }
    }

    TxManager engine;

    if (runTests) {
        quietMode = true;
        cout << "========================================\n";
        cout << "Running Transaction Manager Test Suite\n";
        cout << "========================================\n";

        struct Test {
            string name;
            function<void(TxManager&)> fn;
        };

        vector<Test> tests = {
            {"Snapshot Isolation", demoSnapshot},
            {"Shared Locks", demoSharedLocks},
            {"Blocking / SI Snapshot Read", demoBlocking},
            {"Lock Upgrade (S -> X)", demoUpgrade},
            {"Deadlock Detection", demoDeadlock},
            {"Lost Update Prevention (SI)", demoLostUpdate},
            {"Vacuum GC version pruning", demoVacuum}
        };

        int passed = 0;
        for (size_t i = 0; i < tests.size(); ++i) {
            cout << "Test " << (i + 1) << ": " << tests[i].name << " ... ";
            cout.flush();
            try {
                tests[i].fn(engine);
                cout << "\033[32mPASS\033[0m\n";
                passed++;
            } catch (const exception& e) {
                cout << "\033[31mFAIL\033[0m\n";
                cerr << "   ↳ Error: " << e.what() << "\n";
            }
        }

        cout << "----------------------------------------\n";
        cout << "Result: " << passed << " / " << tests.size() << " passed.\n";
        cout << "========================================\n";

        if (passed == static_cast<int>(tests.size())) {
            return 0;
        } else {
            return 1;
        }
    } else {
        // Original Demo Mode
        demoSnapshot(engine);
        demoSharedLocks(engine);
        demoBlocking(engine);
        demoUpgrade(engine);
        demoDeadlock(engine);
        demoLostUpdate(engine);
        demoVacuum(engine);
        return 0;
    }
}