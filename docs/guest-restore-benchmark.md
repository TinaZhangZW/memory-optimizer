# Guest page restoration benchmark

`tests/guest-restore-bench.c` measures first- and second-access latency for a
controlled guest mapping after host-side pageout. Run it as root inside the
guest with 4 KiB pages, access to pagemap PFNs and permission to raise the
memlock limit. It accepts a size from 1 to 256 MiB and one content pattern:

| Pattern | Contents of each 4 KiB page |
| --- | --- |
| sparse | 8 pseudorandom bytes, remainder zero |
| mixed | 2048 pseudorandom bytes, remainder zero |
| random | 4096 pseudorandom bytes |

The mapping uses MADV_NOHUGEPAGE and guest mlock. Measurement metadata is
allocated separately from the target mapping. Guest mlock prevents guest OS
reclaim; the host can still request pageout of QEMU backing pages.

## Build and launch

Build from the memory-optimizer directory:

```bash
gcc -static -O2 -Wall -Wextra -Werror tests/guest-restore-bench.c \
    -o /tmp/guest-restore-bench
```

From the workspace root, upload through the existing VM SSH helper:

```bash
bash scripts/vm-ssh.sh \
    'cat > /tmp/guest-restore-bench && chmod +x /tmp/guest-restore-bench' \
    < /tmp/guest-restore-bench
```

Use a fresh working directory for each process invocation. For example:

```bash
bash scripts/vm-ssh.sh 'bash -s' <<'EOF'
set -euo pipefail
directory=$(mktemp -d /tmp/restore-bench.XXXXXX)
echo "Working directory: $directory"
sudo systemd-run --unit=restore-bench-sparse \
    --property="WorkingDirectory=$directory" \
    --property=LimitMEMLOCK=infinity \
    /tmp/guest-restore-bench 16 sparse
EOF
```

Wait for the ready log before using pages.tsv. That file has no header and
contains `page_index guest_physical_address`, one row per target page. It is
published by rename after the entire export is written. GPAs are not host
PFNs or QEMU HVAs: resolve them through the current QEMU mapping information
before passing an HVA allowlist to task-refs. Do not reuse mappings from a
previous process allocation or VM instance.

## Command and result protocol

Only one controller and one benchmark process may use a working directory.
Each command contains a unique tag of 1 to 63 characters from ASCII letters,
digits, underscore and hyphen, followed by exactly one newline. Publish it
atomically with rename so the guest cannot read a partly written command.
In the guest, with the benchmark working directory selected:

```bash
printf '%s\n' round1 > command.tmp
mv command.tmp command
```

A tag must never be reused for a new measurement. The benchmark rejects tags
whose latency or done files already exist. A fresh directory also avoids
stale pages.tsv, pending commands and results from a prior process. Command
consumption is not a durable queue; if the process fails, start a fresh run
rather than inferring successful measurement from a missing command file.

The guest reads one byte from each page in shuffled order twice, then checks
full-page hashes. It publishes these files:

- `<tag>.latency.tsv`: page index, first_ns and second_ns, with a header.
- `<tag>.done`: pages, first_total_ns, second_total_ns, errors and pattern.

Both files are written to temporary files, closed successfully and renamed.
The done file is published last: once a controller observes its existence,
the complete latency file is available. Close or write errors prevent done
publication. This provides atomic visibility, not crash durability; no fsync
is performed. Polling must also check service liveness and have a timeout.
A published done file with nonzero errors reports a failed content check.

## Host pageout and validation

First collect a resident baseline with a unique command tag. Before each
restoration round, use the exported target addresses to request host-side
pageout, then read QEMU pagemap to identify which target pages actually have
swapped PTEs. Only afterward send a new command tag to the guest.

Filter per-page first-access statistics by that exact swapped-page set.
Successful process_madvise return values alone do not prove pageout. After
the command completes, require errors=0, all target mappings present again,
and consistent zswap and swap-I/O counter deltas. Check host zswap enabled,
compressor selection and available swap slots separately.

Per-access CLOCK_MONOTONIC_RAW durations include timing overhead and elapsed
fault/restore work; they are not isolated decompression CPU costs. Whole-pass
times include the loop and bookkeeping. Swapcache and readahead can change
which individual accesses incur work. Sparse or incompressible contents may
follow different kernel storage paths, so report the pattern explicitly.

Stop the test service after collection:

```bash
bash scripts/vm-ssh.sh 'sudo systemctl stop restore-bench-sparse'
```

## Protocol regression test

```bash
bash tests/run-guest-restore-protocol.sh
```

This unprivileged test checks strict tag parsing, rejects existing results,
pauses a writer during close to check that done remains invisible, and
injects a close error to confirm no completion is published. It does not
perform real pageout or measure VM restoration performance.
