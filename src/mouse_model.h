// Raw mouse counts travelling from their source to the camera updates. No Windows or game
// dependencies: mouse-model-test.cpp runs this on every build.
//
// Sources only add to running totals. Each camera keeps its own read position, so a count reaches
// every camera exactly once however the updates and the polls interleave, and two cameras blending
// in a transition both turn. Counts are dropped only when they can no longer belong to the current
// movement:
//  - the camera wasn't updating (menu, cutscene, another camera mode): the gap exceeds maxGap;
//  - the game hasn't delivered input for a while (input blocked, or a few counts left over at the
//    end of a movement): they waited longer than the idle limit.
// A count may arrive between the game's poll and the camera update, so the game delivers it one
// update later. It must wait for that update, or slow single-count movement would be lost.
#pragma once

#include <atomic>

namespace acb {

struct CountPair {
    long x = 0;
    long y = 0;
};

// Totals wrap around; differences of 32-bit values stay correct across the wrap.
struct CountSnapshot {
    CountPair raw;
    CountPair dinput;
};

class CameraCounts {
public:
    static constexpr int kCameras = 8;

    struct Taken {
        CountSnapshot counts;
        bool apply = false;     // the game delivered input in this update: apply these counts
        int gapDrops = 0;       // counts discarded because the camera wasn't updating
        int idleDrops = 0;      // counts discarded because the game delivered no input for too long
    };

    // Times in any monotonic unit. delivered: the game's own read of this update had real input.
    Taken Take(const void* camera, const CountSnapshot& totals, bool delivered, long long now, long long maxGap,
               long long minIdle) {
        Taken result;
        while (lock_.test_and_set(std::memory_order_acquire)) {
        }
        Entry* entry = Find(camera);
        const bool wasRunning = entry != nullptr && now - entry->lastUpdate <= maxGap;
        if (entry == nullptr) entry = Replace(camera);
        if (!wasRunning) {
            if (entry->seen && HasPending(*entry, totals)) result.gapDrops = 1;
            Skip(*entry, totals);
            entry->pendingSince = 0;
            entry->seen = true;
        }
        const long long sinceLast = wasRunning ? now - entry->lastUpdate : 0;
        entry->lastUpdate = now;

        if (delivered) {
            result.counts.raw = Difference(totals.raw, entry->seenTotals.raw);
            result.counts.dinput = Difference(totals.dinput, entry->seenTotals.dinput);
            result.apply = true;
            Skip(*entry, totals);
            entry->pendingSince = 0;
        } else if (HasPending(*entry, totals)) {
            if (entry->pendingSince == 0) {
                entry->pendingSince = now;
            } else {
                long long idleLimit = sinceLast * 3;
                if (idleLimit < minIdle) idleLimit = minIdle;
                if (now - entry->pendingSince > idleLimit) {
                    result.idleDrops = 1;
                    Skip(*entry, totals);
                    entry->pendingSince = 0;
                }
            }
        }
        lock_.clear(std::memory_order_release);
        return result;
    }

    int ActiveCameras(long long now, long long maxGap) {
        while (lock_.test_and_set(std::memory_order_acquire)) {
        }
        int active = 0;
        for (const Entry& e : entries_) {
            if (e.camera != nullptr && now - e.lastUpdate <= maxGap) ++active;
        }
        lock_.clear(std::memory_order_release);
        return active;
    }

private:
    struct Entry {
        const void* camera = nullptr;
        CountSnapshot seenTotals;
        long long lastUpdate = 0;
        long long pendingSince = 0;
        bool seen = false;
    };

    static CountPair Difference(const CountPair& a, const CountPair& b) {
        CountPair d;
        d.x = long(static_cast<unsigned long>(a.x) - static_cast<unsigned long>(b.x));
        d.y = long(static_cast<unsigned long>(a.y) - static_cast<unsigned long>(b.y));
        return d;
    }

    static bool HasPending(const Entry& e, const CountSnapshot& totals) {
        return totals.raw.x != e.seenTotals.raw.x || totals.raw.y != e.seenTotals.raw.y ||
               totals.dinput.x != e.seenTotals.dinput.x || totals.dinput.y != e.seenTotals.dinput.y;
    }

    static void Skip(Entry& e, const CountSnapshot& totals) { e.seenTotals = totals; }

    Entry* Find(const void* camera) {
        for (Entry& e : entries_) {
            if (e.camera == camera) return &e;
        }
        return nullptr;
    }

    Entry* Replace(const void* camera) {
        Entry* oldest = &entries_[0];
        for (Entry& e : entries_) {
            if (e.camera == nullptr) {
                oldest = &e;
                break;
            }
            if (e.lastUpdate < oldest->lastUpdate) oldest = &e;
        }
        *oldest = Entry{};
        oldest->camera = camera;
        return oldest;
    }

    Entry entries_[kCameras];
    std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
};

} // namespace acb
