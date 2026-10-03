// SPDX-License-Identifier: GPL-2.0
// Populate and retain GiB of guest RAM, repeatedly reading a small hot subset.
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define PAGE_BYTES 4096UL
#define GIB_BYTES (1UL << 30)
#define HOT_BYTES (256UL << 20)
#define MAX_GIB 62UL
#define REPORT_SECONDS 10
#define SWEEP_PAUSE_US 5000

static volatile sig_atomic_t stopping;
static volatile unsigned long sink;

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static int parse_size(int argc, char **argv, unsigned long *gib)
{
    char *end;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s GiB (1-%lu)\n", argv[0], MAX_GIB);
        return -1;
    }

    errno = 0;
    *gib = strtoul(argv[1], &end, 10);
    if (errno || end == argv[1] || *end || *gib < 1 || *gib > MAX_GIB ||
        *gib > SIZE_MAX / GIB_BYTES) {
        fprintf(stderr, "Invalid memory size: %s\n", argv[1]);
        return -1;
    }
    return 0;
}

static void populate_pages(uint8_t *data, size_t bytes)
{
    for (size_t offset = 0; offset < bytes && !stopping; offset += PAGE_BYTES) {
        // A nonzero write faults in each page instead of sharing the zero page.
        *(volatile uint8_t *)(data + offset) =
            (uint8_t)(1 + (offset / PAGE_BYTES) % 251);

        if ((offset + PAGE_BYTES) % GIB_BYTES == 0) {
            printf("populated_gib=%zu\n", (offset + PAGE_BYTES) / GIB_BYTES);
            fflush(stdout);
        }
    }
}

static void run_hot_workload(const uint8_t *data)
{
    unsigned long sweeps = 0;
    time_t last = time(NULL);

    // Leave the rest of the allocation untouched so it can become cold.
    while (!stopping) {
        for (size_t offset = 0; offset < HOT_BYTES; offset += PAGE_BYTES)
            sink += *(const volatile uint8_t *)(data + offset);
        ++sweeps;

        time_t now = time(NULL);
        if (now - last >= REPORT_SECONDS) {
            printf("hot_sweeps=%lu elapsed_seconds=%ld\n",
                   sweeps, (long)(now - last));
            fflush(stdout);
            sweeps = 0;
            last = now;
        }
        usleep(SWEEP_PAUSE_US);
    }
}

int main(int argc, char **argv)
{
    unsigned long gib;
    int result = 1;
    struct rlimit lim = {RLIM_INFINITY, RLIM_INFINITY};

    if (parse_size(argc, argv, &gib))
        return 2;
    if (sysconf(_SC_PAGESIZE) != PAGE_BYTES) {
        fprintf(stderr, "This workload requires 4 KiB pages\n");
        return 2;
    }
    size_t bytes = (size_t)gib * GIB_BYTES;

    if (setrlimit(RLIMIT_MEMLOCK, &lim)) {
        perror("memlock limit");
        return 1;
    }
    if (signal(SIGTERM, stop) == SIG_ERR || signal(SIGINT, stop) == SIG_ERR) {
        perror("signal");
        return 1;
    }

    uint8_t *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    if (madvise(data, bytes, MADV_NOHUGEPAGE)) {
        perror("nohugepage");
        goto out_unmap;
    }

    populate_pages(data, bytes);
    if (stopping) {
        result = 0;
        goto out_unmap;
    }

    // Lock inside the guest to keep its OS from reclaiming the cold pages.
    if (mlock(data, bytes)) {
        perror("mlock");
        goto out_unmap;
    }
    printf("ready pid=%d resident_gib=%lu hot_mib=%lu\n",
           getpid(), gib, HOT_BYTES >> 20);
    fflush(stdout);

    run_hot_workload(data);
    result = 0;

out_unmap:
    // Unmapping also releases any memory locks acquired above.
    munmap(data, bytes);
    return result;
}
