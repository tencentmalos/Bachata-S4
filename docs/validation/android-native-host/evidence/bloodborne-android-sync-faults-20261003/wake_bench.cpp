// Waker-side CPU cost of waking a sleeping thread (FUTEX_WAKE), by wakee/waker affinity.
// usage: wake_bench <iterations> <waker_mask_hex|-> <wakee_mask_hex|-> [gap_us]
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

static std::atomic<uint32_t> word{0};
static std::atomic<uint32_t> sleeping{0};
static std::atomic<uint32_t> acks{0};
static std::atomic<bool> done{false};

static long futex(std::atomic<uint32_t>* addr, int op, uint32_t val) {
    return syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), op | FUTEX_PRIVATE_FLAG, val,
                   nullptr, nullptr, 0);
}

static int64_t thread_cpu_ns() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

static int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

static void pin(const char* mask) {
    if (!mask || mask[0] == '-')
        return;
    unsigned long bits = strtoul(mask, nullptr, 16);
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 32; ++i)
        if (bits & (1ul << i))
            CPU_SET(i, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        perror("sched_setaffinity");
}

static const char* wakee_mask;

static void* wakee(void*) {
    pin(wakee_mask);
    uint32_t seen = 0;
    while (!done.load()) {
        sleeping.store(1);
        while (word.load() == seen && !done.load())
            futex(&word, FUTEX_WAIT, seen);
        seen = word.load();
        sleeping.store(0);
        acks.fetch_add(1);
    }
    return nullptr;
}

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? atoi(argv[1]) : 2000;
    const char* waker_mask = argc > 2 ? argv[2] : "-";
    wakee_mask = argc > 3 ? argv[3] : "-";
    const int gap_us = argc > 4 ? atoi(argv[4]) : 300;
    pin(waker_mask);
    pthread_t thread;
    pthread_create(&thread, nullptr, wakee, nullptr);
    std::vector<int64_t> cpu, wall;
    for (int i = 0; i < iterations; ++i) {
        // Let the wakee settle into the kernel wait (and its CPU go idle).
        while (sleeping.load() == 0)
            ;
        usleep(gap_us);
        const uint32_t before = acks.load();
        const int64_t c0 = thread_cpu_ns(), w0 = mono_ns();
        word.fetch_add(1);
        futex(&word, FUTEX_WAKE, 1);
        const int64_t c1 = thread_cpu_ns(), w1 = mono_ns();
        cpu.push_back(c1 - c0);
        wall.push_back(w1 - w0);
        while (acks.load() == before)
            ;
    }
    done.store(true);
    word.fetch_add(1);
    futex(&word, FUTEX_WAKE, 1);
    pthread_join(thread, nullptr);
    std::sort(cpu.begin(), cpu.end());
    std::sort(wall.begin(), wall.end());
    auto pct = [](const std::vector<int64_t>& v, double p) { return v[size_t(p * (v.size() - 1))] / 1000.0; };
    double mean = 0;
    for (auto v : cpu)
        mean += v;
    mean /= cpu.size() * 1000.0;
    printf("waker=%s wakee=%s gap=%dus n=%d  waker cpu us: mean %.2f p50 %.2f p90 %.2f p99 %.2f | wall p50 %.2f p90 %.2f\n",
           waker_mask, wakee_mask, gap_us, iterations, mean, pct(cpu, 0.5), pct(cpu, 0.9),
           pct(cpu, 0.99), pct(wall, 0.5), pct(wall, 0.9));
    return 0;
}
