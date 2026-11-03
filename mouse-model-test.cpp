// Tests of src\mouse_model.h: no game, no Windows dependencies.
#include "src/mouse_model.h"

#include <cstdio>
#include <thread>

using acb::CameraCounts;
using acb::CountSnapshot;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr long long kMs = 1000; // time unit: microseconds
constexpr long long kMaxGap = 250 * kMs;
constexpr long long kMinIdle = 50 * kMs;

const int kCamera = 1;
const int kOtherCamera = 2;

// One second of mouse at countsPerSecond, polled and consumed once per frame. A real-time turn per
// count must not depend on the frame rate: every count arrives exactly once.
long Simulate(int fps, long countsPerSecond) {
    CameraCounts counts;
    CountSnapshot totals;
    long applied = 0;
    long long now = 1000000;
    const long long frame = 1000000 / fps;
    long produced = 0;
    long polled = 0;
    for (long long t = 0; t < 1000000; t += frame) {
        now += frame;
        const long target = long((t + frame) * countsPerSecond / 1000000);
        // The poll sees what arrived by now; the camera update comes a little later in the frame.
        const bool delivered = target != polled;
        polled = target;
        produced = target;
        totals.raw.x = produced;
        const CameraCounts::Taken taken = counts.Take(&kCamera, totals, delivered, now, kMaxGap, kMinIdle);
        if (taken.apply) applied += taken.counts.raw.x;
    }
    return applied;
}

void TestFrameRateIndependence() {
    const int rates[] = {20, 30, 60, 144, 300, 777};
    const long speeds[] = {150, 1500, 8000, 30000};
    for (long speed : speeds) {
        for (int fps : rates) {
            // The first update of a camera starts its read position, so one frame is not counted.
            const long applied = Simulate(fps, speed);
            const long firstFrame = speed / fps + 1;
            if (applied < speed - firstFrame - 1 || applied > speed) {
                printf("FAIL: %d fps at %ld counts/s applied %ld\n", fps, speed, applied);
                ++g_failures;
            }
        }
    }
}

void TestCameraNotRunning() {
    CameraCounts counts;
    CountSnapshot totals;
    long long now = 1000 * kMs;
    counts.Take(&kCamera, totals, false, now, kMaxGap, kMinIdle);
    // A menu is open for a second while the mouse moves.
    totals.raw.x = 5000;
    now += 1000 * kMs;
    CameraCounts::Taken taken = counts.Take(&kCamera, totals, true, now, kMaxGap, kMinIdle);
    Check(taken.counts.raw.x == 0, "counts from while the camera wasn't running must not be applied");
    Check(taken.gapDrops == 1, "the gap drop is reported");
    // The next movement works normally.
    totals.raw.x = 5010;
    now += 3 * kMs;
    taken = counts.Take(&kCamera, totals, true, now, kMaxGap, kMinIdle);
    Check(taken.apply && taken.counts.raw.x == 10, "counts after the gap are applied");
}

void TestCountArrivingBeforeTheGameDeliversIt() {
    CameraCounts counts;
    CountSnapshot totals;
    long long now = 1000 * kMs;
    counts.Take(&kCamera, totals, false, now, kMaxGap, kMinIdle);
    now += 500 * kMs; // idle for a long time...
    // ...the camera was still updating every few ms.
    CameraCounts counts2;
    CountSnapshot t2;
    long long n = 1000 * kMs;
    for (int i = 0; i < 100; ++i) {
        n += 3 * kMs;
        counts2.Take(&kCamera, t2, false, n, kMaxGap, kMinIdle);
    }
    // One count arrives after the poll of this frame: not delivered yet.
    t2.raw.x = 1;
    n += 3 * kMs;
    CameraCounts::Taken taken = counts2.Take(&kCamera, t2, false, n, kMaxGap, kMinIdle);
    Check(!taken.apply && taken.idleDrops == 0, "a fresh count must wait");
    // The next poll delivers it.
    n += 3 * kMs;
    taken = counts2.Take(&kCamera, t2, true, n, kMaxGap, kMinIdle);
    Check(taken.apply && taken.counts.raw.x == 1, "a slow single count must not be lost");
}

void TestBlockedInputIsNotReplayed() {
    CameraCounts counts;
    CountSnapshot totals;
    long long now = 1000 * kMs;
    counts.Take(&kCamera, totals, false, now, kMaxGap, kMinIdle);
    // The game blocks the camera input for a second while the mouse keeps moving.
    int drops = 0;
    for (int i = 0; i < 333; ++i) {
        now += 3 * kMs;
        totals.raw.x += 10;
        drops += counts.Take(&kCamera, totals, false, now, kMaxGap, kMinIdle).idleDrops;
    }
    Check(drops >= 15, "counts while the input is blocked are dropped");
    now += 3 * kMs;
    totals.raw.x += 10;
    const CameraCounts::Taken taken = counts.Take(&kCamera, totals, true, now, kMaxGap, kMinIdle);
    Check(taken.apply && taken.counts.raw.x <= 10 * (kMinIdle / (3 * kMs) + 2),
          "at most the idle limit's worth of counts is applied after unblocking");
}

void TestTwoCameras() {
    CameraCounts counts;
    CountSnapshot totals;
    long long now = 1000 * kMs;
    counts.Take(&kCamera, totals, false, now, kMaxGap, kMinIdle);
    counts.Take(&kOtherCamera, totals, false, now, kMaxGap, kMinIdle);
    long a = 0;
    long b = 0;
    for (int i = 0; i < 100; ++i) {
        now += 3 * kMs;
        totals.raw.x += 7;
        a += counts.Take(&kCamera, totals, true, now, kMaxGap, kMinIdle).counts.raw.x;
        b += counts.Take(&kOtherCamera, totals, true, now, kMaxGap, kMinIdle).counts.raw.x;
    }
    Check(a == 700 && b == 700, "two blending cameras both get every count exactly once");
    Check(counts.ActiveCameras(now, kMaxGap) == 2, "both cameras are active");
}

void TestManyCamerasRecycleEntries() {
    CameraCounts counts;
    CountSnapshot totals;
    long long now = 1000 * kMs;
    int dummies[CameraCounts::kCameras * 3];
    for (int& d : dummies) {
        now += kMs;
        counts.Take(&d, totals, false, now, kMaxGap, kMinIdle);
    }
    Check(counts.ActiveCameras(now, kMaxGap) == CameraCounts::kCameras, "old camera entries are recycled");
}

void TestWrapAround() {
    CameraCounts counts;
    CountSnapshot totals;
    totals.raw.x = 0x7FFFFFF0;
    long long now = 1000 * kMs;
    counts.Take(&kCamera, totals, false, now, kMaxGap, kMinIdle);
    totals.raw.x = long(0x7FFFFFF0ul + 0x20ul); // past LONG_MAX
    now += 3 * kMs;
    const CameraCounts::Taken taken = counts.Take(&kCamera, totals, true, now, kMaxGap, kMinIdle);
    Check(taken.counts.raw.x == 0x20, "totals may wrap around");
}

void TestTwoThreads() {
    CameraCounts counts;
    std::atomic<long> total{0};
    std::atomic<bool> done{false};
    const long kCounts = 2000000;
    std::thread source([&] {
        for (long i = 0; i < kCounts; ++i) total.fetch_add(1, std::memory_order_relaxed);
        done = true;
    });
    long applied = 0;
    long long now = 1000 * kMs;
    CountSnapshot totals;
    counts.Take(&kCamera, totals, false, now, kMaxGap, kMinIdle);
    for (;;) {
        const bool finished = done.load();
        totals.raw.x = total.load(std::memory_order_relaxed);
        now += 1;
        applied += counts.Take(&kCamera, totals, true, now, kMaxGap, kMinIdle).counts.raw.x;
        if (finished) break;
    }
    source.join();
    totals.raw.x = total.load();
    now += 1;
    applied += counts.Take(&kCamera, totals, true, now, kMaxGap, kMinIdle).counts.raw.x;
    Check(applied == kCounts, "no count is lost or doubled across threads");
}

} // namespace

int main() {
    TestFrameRateIndependence();
    TestCameraNotRunning();
    TestCountArrivingBeforeTheGameDeliversIt();
    TestBlockedInputIsNotReplayed();
    TestTwoCameras();
    TestManyCamerasRecycleEntries();
    TestWrapAround();
    TestTwoThreads();
    if (g_failures == 0) printf("ok: mouse model\n");
    return g_failures == 0 ? 0 : 1;
}
