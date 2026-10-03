# Idle bitmap I/O statistics

`task-refs --scan idle-bitmap` measures bitmap writes and reads separately. The measurement supports word-at-a-time and contiguous batched I/O for evaluating hardware-assisted access tracking. The default allowlist limit remains 262,144 pages (1 GiB). An explicit `--bitmap-max-pages N` override permits up to 16,777,216 pages (64 GiB).

## Run a measurement

From the workspace root, use a validated HVA allowlist and its guest RAM range:

```bash
sudo ./memory-optimizer/task-refs \
  -p "$QEMU_PID" \
  --scan idle-bitmap --backend none \
  --addresses "$HVA_LIST" --ram-range "$RAM_RANGE" \
  -l 10 -i 0.1 -o /tmp/cold-scan
```

This creates the existing `/tmp/cold-scan` histogram and `/tmp/cold-scan.pages.tsv` page results, plus `/tmp/cold-scan.bitmap-stats.tsv`. An aggregate timing summary is also printed to stderr. `--backend none` avoids pageout; sampling still marks idle and updates access state.

The statistics TSV contains one row per started round, numbered from 1, and a `total` row. It is written after sampling, outside the timed phases. Partial statistics are retained on scan failure when the output can be written. Check the process exit status and stderr as well as the table. Failure to write statistics makes the scan fail and disables pageout from that scan.

## Contiguous batching

Add `--bitmap-batch-bytes 64`, `512`, or `4096` to compare with the default `8` bytes. Values must be multiples of eight in [8, 4096]. This is a maximum transfer size: only consecutive selected bitmap words are merged, with no gap filling or range padding. Each write retains its per-word target mask. Thus batching does not add PFN positions to read coverage. A sparse layout may provide little opportunity for batching, even with a 4096-byte limit.

Buffers are built before the write timer and unpacked after the read timer. Each timed phase iterates the same batch representation, including for the eight-byte baseline. Short transfers remain fail-closed: the scan records the transfer and fails rather than reporting incomplete data as a complete sample.

The prepared comparison is `results/idle-bitmap-batching-20261001/run.sh` in the workspace. It uses the previously validated 6144-page allowlist for QEMU PID 22720, performs two warmup rounds, and rotates four buffer sizes through four passes of ten rounds each. It preserves existing results and performs no pageout. `summarize.sh` reports averages and PFN-count/coverage ranges; raw round records retain all timings and validity counts. HVA targets are identical, but PFNs are re-resolved each round and may migrate. Compare coverage and page results before attributing differences to batching. This measures the batching benefit, not isolated syscall entry/exit latency.

## Parallel bitmap scanning

Use `--bitmap-threads 1`, `2`, or `4` to compare the same workload; the accepted
range is 1..64 and the default is 1. For example:

```bash
sudo ./memory-optimizer/task-refs \
  -p "$QEMU_PID" --scan idle-bitmap --backend none \
  --addresses "$HVA_LIST" --ram-range "$RAM_RANGE" \
  --bitmap-batch-bytes 4096 --bitmap-threads 4 \
  -l 10 -i 0.1 -o /tmp/cold-scan-4threads
```

PFN discovery and grouping happen on the calling thread. The scanner first builds
the same consecutive-word batches as the single-thread implementation, then
assigns non-overlapping contiguous slices of that batch list to workers, balanced
by batch count. It does not split batches to occupy extra workers: coverage,
write masks, bytes and syscall counts remain unchanged for stable target PFNs.
A round with fewer batches than configured threads has fewer active workers.
The configured page limit applies to the whole scan, not to each worker. It defaults to 262,144 pages / 1 GiB.

Each worker has a separately opened bitmap descriptor, avoiding the kernfs mutex
shared by an individual open file instance. Workers persist across all rounds in
one scan. The one-thread mode runs directly on the calling thread. All writes
finish before the observation wait, and all reads finish before mapping
revalidation. Pageout remains serial and retains the existing validation rules.
One failed worker causes the scan to fail after the other workers finish the
current phase; no read phase follows a failed write phase. Partial I/O statistics
are preserved, and failed scans cannot supply pageout candidates.

Wall time covers phase dispatch through completion, including scheduling and
synchronization. CPU time is the coordinator CPU time over that phase plus each
active worker bitmap-loop CPU time. Worker synchronization outside the timed loop,
thread creation/destruction, descriptor opens, and result aggregation are not
included in the CPU counter; it is not whole-process CPU usage. Thread creation
occurs before the rounds, so warmup rounds are useful for first-dispatch effects.
The workers are not CPU-pinned. Different physical ranges can still contend on
host/KVM locks or cause remote TLB work, so speedup is not assumed to be linear.
Idle tracking state remains global; avoid overlapping independent scans.

`sys-refs` retains its existing `/proc/PID/idle_pages` worker path; this option is
currently wired only through `task-refs` idle-bitmap scanning.

## Explicit larger scans

`--bitmap-max-pages N` accepts 1..16777216, with a default of 262144. The
upper bound corresponds to 64 GiB of listed 4 KiB HVA pages. It changes only
the allowed input size, not eligibility, address-range checks, mapping
revalidation or I/O batching. The input still must be an explicit HVA list
within validated private anonymous RAM. Supplying an entire RAMBlock range
includes unused guest pages that remain resident on the host; pagemap does
not identify guest-OS allocation state.

A larger scan marks all its eligible pages, waits once, then reads all its
bitmap batches. This differs from independent 1 GiB invocations with a wait
per invocation. PFN lookup and final per-page output remain serial and can
be substantial: retain whole-command timings as well as bitmap-phase times.
The existing in-memory page samples and PFN associations, plus text output,
grow with input size; full scans can consume GiB of scanner memory and hundreds
of MiB of output. A single long scan also has different start/end timestamps
for individual pages; it is not a simultaneous snapshot.

## Fields

| Field | Meaning |
| --- | --- |
| `round` | Round number, or `total` |
| `complete` | Round finished including mapping revalidation; on the total row, the scan succeeded before statistics reporting |
| `requested_hvas` | Input HVA pages examined by the round |
| `eligible_hvas` | HVA pages eligible at the start of the round |
| `unique_pfns` | Distinct target PFNs after grouping; aliases count once |
| `bitmap_words` | Distinct 64-PFN bitmap words containing those targets |
| `planned_pfn_positions` | `bitmap_words * 64`; planned read coverage including unrelated positions |
| `valid_hvas` | HVA pages passing the ending mapping and eligibility checks |
| `idle_hvas` | Valid HVA samples whose returned idle bit is 1 |
| `accessed_hvas` | Valid HVA samples whose returned idle bit is 0 |
| `write_calls`, `read_calls` | Actual bitmap syscall attempts, including failed and short calls |
| `write_bytes`, `read_bytes` | Sum of nonnegative syscall return values, including short transfers |
| `write_short_calls`, `read_short_calls` | Nonnegative returns smaller than the requested transfer size, including zero |
| `write_errors`, `read_errors` | Negative syscall returns |
| `write_wall_ns`, `read_wall_ns` | Elapsed time around the respective bitmap loop, using `CLOCK_MONOTONIC` |
| `write_cpu_ns`, `read_cpu_ns` | Coordinator plus active-worker bitmap-loop CPU time, using `CLOCK_THREAD_CPUTIME_ID`; see timing boundaries |
| `batch_bytes` | Configured maximum transfer size; repeated unchanged in the total row |
| `threads` | Configured worker count; repeated unchanged in the total row |
| `active_workers` | Workers assigned at least one batch; total row sums worker participation across rounds |
| `wait_wall_ns` | Elapsed observation-wait phase, reported separately |

Except for the configuration fields `batch_bytes` and `threads`, all numerical counters and durations are summed in the total row. In particular, total `unique_pfns` is a sum of per-round target counts, not a deduplicated PFN count across the entire scan. Total `idle_hvas` counts idle observations, not distinct pages cold throughout all rounds. Use the page TSV (`eligible=1`, `refs=0`) to identify fully sampled pages with no observed access across the complete scan.

On failure, planned coverage can exceed completed read coverage. Do not interpret `planned_pfn_positions` as an exact count of kernel pages inspected. For successful reads, `read_bytes * 8` gives the number of PFN positions returned, but invalid PFNs and skipped folios do not incur the same kernel work as eligible pages.

## Timing boundaries

```text
Resolve HVAs and build PFN masks         outside bitmap timers
Write all bitmap words                  write_wall_ns / write_cpu_ns
Wait for workload activity              wait_wall_ns
Read all bitmap words                   read_wall_ns / read_cpu_ns
Validate mappings and update samples    outside bitmap timers
Write results and statistics            outside bitmap timers
```

Timers surround each whole phase, not each syscall. The measured bitmap phases include syscall overhead, kernel work, batch iteration and lightweight counter updates. They exclude pagemap/kpageflags reads, PFN grouping, deliberate sleep, final validation, and output formatting. Fixed clock-reading overhead remains; even an empty phase can show a small nonzero duration. Error paths include work performed up to failure and exception handling inside the phase.

Wall time includes scheduling delays. Thread CPU time includes userspace and kernel execution charged to the measured scanner/worker sections, but does not capture all remote CPU work, TLB disruption or guest slowdown. Measure guest impact separately when comparing with hardware.

## Interpreting the number of operations

With the default eight-byte limit, the implementation performs one eight-byte write and one eight-byte read per selected bitmap word per successful round:

```text
words_per_round = count_distinct(target_PFN / 64)
write_calls_per_round = words_per_round
read_calls_per_round = words_per_round
```

These counts depend on eligible resident host backing and physical placement. They are not a measurement of how much guest RAM currently has an EPT/NPT entry. A resident guest backing page can remain a target even when its secondary mapping is absent. KVM mapping topology can affect kernel work and latency without changing the number of userspace bitmap calls.

For a stable set occupying 1000 bitmap words and 10 rounds, the expected successful counts are 10,000 writes and 10,000 reads. Each direction transfers 80,000 bytes. This is an arithmetic example, not a benchmark result. The interface is sampled explicitly; it does not push notifications when a page becomes cold.

Use `unique_pfns` and `planned_pfn_positions` to explain sparse physical coverage. Compare the same target set and workload across batch sizes. With batching, calls per direction equal the sum of ceil(run_words / (batch_bytes / 8)) over consecutive selected-word runs. Report actual coverage: this POC still supports only ordinary eligible 4 KiB pages with a default 1 GiB allowlist cap. For an explicitly authorized full-RAM benchmark, `--bitmap-max-pages 16777216` permits a 64 GiB address list. Nonresident and ineligible pages are still skipped.

## Validation

`bash tests/run-idle-bitmap-scan.sh` uses mocked kernel interfaces. It checks deterministic per-round and total wall/CPU timing, exclusion of the observation wait, word grouping, PFN aliases, idle/accessed sample counts, failures, short transfers, output failure, and reset between scans. The existing real-clock signal test also verifies that interruption does not shorten the observation window. Parallel tests additionally verify actual overlapping I/O, independent descriptors, phase ordering, no duplicate coverage, deterministic summed worker CPU time, idle workers, invalid thread counts, and cleanup on worker/open failures. These are correctness tests, not measurements of real idle-bitmap kernel performance.
