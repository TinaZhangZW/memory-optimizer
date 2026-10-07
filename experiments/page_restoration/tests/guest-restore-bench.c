// SPDX-License-Identifier: GPL-2.0
// Controlled guest pages and first-touch latency; run as root in the guest.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define PAGE_BYTES 4096UL
#define MAX_MIB 1024UL
#define TAG_BYTES 64
#define PATH_BYTES 128
#define PFN_MASK ((1ULL << 55) - 1)

struct benchmark {
    uint8_t *data;
    size_t bytes;
    size_t pages;
    size_t random_words;
    const char *pattern;
    int batch_only;
    uint64_t *expected;
    uint64_t *first;
    uint64_t *second;
    size_t *order;
};

static volatile sig_atomic_t stopping;
static volatile uint64_t sink;

static void stop(int sig)
{
    (void)sig;
    stopping = 1;
}

static uint64_t now(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts)) {
        perror("clock_gettime");
        abort();
    }
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static uint64_t random64(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static uint64_t page_hash(const uint8_t *page)
{
    uint64_t hash = 1469598103934665603ULL;

    for (size_t i = 0; i < PAGE_BYTES; ++i)
        hash = (hash ^ page[i]) * 1099511628211ULL;
    return hash;
}

static int parse_options(int argc, char **argv, struct benchmark *bench)
{
    char *end;
    unsigned long mib;

    if ((argc != 3 && argc != 4) || sysconf(_SC_PAGESIZE) != PAGE_BYTES) {
        fprintf(stderr, "Usage: %s MiB sparse|mixed|random [latency|batch] (4 KiB pages)\n", argv[0]);
        return -1;
    }
    if (argc == 4) {
        if (!strcmp(argv[3], "batch"))
            bench->batch_only = 1;
        else if (strcmp(argv[3], "latency"))
            return -1;
    }
    errno = 0;
    mib = strtoul(argv[1], &end, 10);
    if (errno || end == argv[1] || *end || mib < 1 || mib > MAX_MIB)
        return -1;

    bench->pattern = argv[2];
    if (!strcmp(bench->pattern, "sparse"))
        bench->random_words = 1;
    else if (!strcmp(bench->pattern, "mixed"))
        bench->random_words = 256;
    else if (!strcmp(bench->pattern, "random"))
        bench->random_words = 512;
    else
        return -1;
    bench->bytes = (size_t)mib << 20;
    bench->pages = bench->bytes / PAGE_BYTES;
    return 0;
}

static void release_pages(struct benchmark *bench)
{
    if (bench->data && bench->data != MAP_FAILED)
        munmap(bench->data, bench->bytes);
    free(bench->expected);
    free(bench->first);
    free(bench->second);
    free(bench->order);
}

static int prepare_pages(struct benchmark *bench)
{
    uint64_t rng = 0x912b473caf31ULL;
    size_t count = bench->pages;

    bench->data = mmap(NULL, bench->bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (bench->data == MAP_FAILED)
        return -1;
    if (madvise(bench->data, bench->bytes, MADV_NOHUGEPAGE) ||
        mlock(bench->data, bench->bytes))
        return -1;

    bench->expected = calloc(count, sizeof(*bench->expected));
    bench->first = calloc(count, sizeof(*bench->first));
    bench->second = calloc(count, sizeof(*bench->second));
    bench->order = calloc(count, sizeof(*bench->order));
    if (!bench->expected || !bench->first || !bench->second || !bench->order)
        return -1;

    for (size_t i = 0; i < count; ++i) {
        uint64_t *words = (uint64_t *)(bench->data + i * PAGE_BYTES);

        for (size_t k = 0; k < bench->random_words; ++k)
            words[k] = random64(&rng);
        bench->expected[i] = page_hash((uint8_t *)words);
        bench->order[i] = i;
    }
    for (size_t i = count - 1; i > 0; --i) {
        size_t j = random64(&rng) % (i + 1);
        size_t tmp = bench->order[i];

        bench->order[i] = bench->order[j];
        bench->order[j] = tmp;
    }

    // Metadata stays outside the mapping exported for host pageout.
    if (mlock(bench->expected, count * sizeof(*bench->expected)) ||
        mlock(bench->first, count * sizeof(*bench->first)) ||
        mlock(bench->second, count * sizeof(*bench->second)) ||
        mlock(bench->order, count * sizeof(*bench->order)))
        return -1;
    return 0;
}

static int publish(FILE *file, const char *temporary, const char *destination)
{
    int failed = ferror(file);

    if (fclose(file))
        failed = 1;
    if (failed) {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, destination)) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int export_gpas(const struct benchmark *bench)
{
    int fd = open("/proc/self/pagemap", O_RDONLY);
    FILE *file;

    if (fd < 0)
        return -1;
    file = fopen("pages.tmp", "w");
    if (!file) {
        close(fd);
        return -1;
    }
    for (size_t i = 0; i < bench->pages; ++i) {
        uint64_t entry;
        off_t offset = ((uintptr_t)bench->data / PAGE_BYTES + i) * sizeof(entry);

        if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry) ||
            !(entry & (1ULL << 63)) || (entry & (1ULL << 62)) || !(entry & PFN_MASK)) {
            fprintf(stderr, "Cannot resolve guest PFN for page %zu\n", i);
            fclose(file);
            close(fd);
            unlink("pages.tmp");
            return -1;
        }
        fprintf(file, "%zu 0x%" PRIx64 "\n", i,
                (uint64_t)((entry & PFN_MASK) * PAGE_BYTES));
    }
    close(fd);
    return publish(file, "pages.tmp", "pages.tsv");
}

// The host publishes a nonempty, newline-terminated tag by renaming command.tmp.
static int read_command(char tag[TAG_BYTES])
{
    FILE *file = fopen("command", "r");
    char line[TAG_BYTES + 2];
    int valid;

    if (!file)
        return errno == ENOENT ? 0 : -1;
    valid = fgets(line, sizeof(line), file) != NULL;
    if (valid) {
        size_t length = strlen(line);

        valid = length >= 2 && length <= TAG_BYTES && line[length - 1] == '\n';
        if (valid) {
            line[length - 1] = '\0';
            valid = strspn(line, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
                    == length - 1 && fgetc(file) == EOF && !ferror(file);
        }
    }
    if (fclose(file))
        return -1;
    if (!valid) {
        fprintf(stderr, "Invalid command tag (1-63 allowed characters plus newline)\n");
        return -1;
    }
    strcpy(tag, line);
    return unlink("command") ? -1 : 1;
}

static uint64_t measure_pass(struct benchmark *bench, uint64_t *latencies)
{
    uint64_t begin = now();

    for (size_t j = 0; j < bench->pages; ++j) {
        size_t i = bench->order[j];
        if (bench->batch_only) {
            sink += *(volatile uint8_t *)(bench->data + i * PAGE_BYTES);
            continue;
        }
        uint64_t start = now();
        uint8_t value = *(volatile uint8_t *)(bench->data + i * PAGE_BYTES);
        uint64_t elapsed = now() - start;

        sink += value;
        latencies[i] = elapsed;
    }
    return now() - begin;
}

static int measure_and_publish(struct benchmark *bench, const char *tag)
{
    char latency[PATH_BYTES], done[PATH_BYTES], temporary[PATH_BYTES];
    uint64_t totals[2];
    size_t errors = 0;
    FILE *file;

    snprintf(latency, sizeof(latency), "%s.latency.tsv", tag);
    snprintf(done, sizeof(done), "%s.done", tag);
    // Reusing a tag could let the host read stale completion data.
    if (access(latency, F_OK) == 0 || access(done, F_OK) == 0) {
        fprintf(stderr, "Result tag already exists: %s\n", tag);
        return -1;
    }

    totals[0] = measure_pass(bench, bench->first);
    totals[1] = measure_pass(bench, bench->second);
    // Verify entire pages only after both timed passes.
    for (size_t i = 0; i < bench->pages; ++i)
        errors += page_hash(bench->data + i * PAGE_BYTES) != bench->expected[i];

    snprintf(temporary, sizeof(temporary), "%s.latency.tsv.tmp", tag);
    file = fopen(temporary, "wx");
    if (!file)
        return -1;
    fprintf(file, "page\tfirst_ns\tsecond_ns\n");
    for (size_t i = 0; !bench->batch_only && i < bench->pages; ++i)
        fprintf(file, "%zu\t%" PRIu64 "\t%" PRIu64 "\n",
                i, bench->first[i], bench->second[i]);
    if (publish(file, temporary, latency))
        return -1;

    snprintf(temporary, sizeof(temporary), "%s.done.tmp", tag);
    file = fopen(temporary, "wx");
    if (!file)
        return -1;
    fprintf(file, "pages=%zu first_total_ns=%" PRIu64 " second_total_ns=%" PRIu64
            " errors=%zu pattern=%s mode=%s\n", bench->pages, totals[0], totals[1], errors,
            bench->pattern, bench->batch_only ? "batch" : "latency");
    if (publish(file, temporary, done))
        return -1;
    return errors ? -1 : 0;
}

int main(int argc, char **argv)
{
    struct benchmark bench = {0};
    struct rlimit lim = {RLIM_INFINITY, RLIM_INFINITY};
    int result = 1;

    if (parse_options(argc, argv, &bench))
        return 2;
    if (setrlimit(RLIMIT_MEMLOCK, &lim) ||
        signal(SIGTERM, stop) == SIG_ERR || signal(SIGINT, stop) == SIG_ERR) {
        perror("workload setup");
        return 1;
    }
    if (prepare_pages(&bench) || export_gpas(&bench)) {
        perror("page preparation");
        goto out;
    }
    for (int i = 0; i < 100; ++i)
        sink += now();
    printf("ready pid=%d pages=%zu pattern=%s\n", getpid(), bench.pages, bench.pattern);
    fflush(stdout);

    while (!stopping) {
        char tag[TAG_BYTES];
        int command = read_command(tag);

        if (command < 0)
            goto out;
        if (!command) {
            usleep(10000);
            continue;
        }
        if (measure_and_publish(&bench, tag))
            goto out;
    }
    result = 0;
out:
    release_pages(&bench);
    return result;
}
