/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (c) 2018 Intel Corporation
 *
 * Authors: Fengguang Wu <fengguang.wu@intel.com>
 *          Peng Bo <bo2.peng@intel.com>
 *          Yao Yuan <yuan.yao@intel.com>
 *          Liu Jingqi <jingqi.liu@intel.com>
 */

#include <stdio.h>
#include <sys/types.h>
#include <getopt.h>

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#include <iostream>

#include "Option.h"
#include "ProcMaps.h"
#include "ProcIdlePages.h"
#include "EPTScan.h"
#include "EPTMigrate.h"
#include "lib/debug.h"
#include "version.h"

using namespace std;

Option option;

int debug_level()
{
  return option.debug_level;
}

static const struct option opts[] = {
  {"pid",       required_argument,  NULL, 'p'},
  {"interval",  required_argument,  NULL, 'i'},
  {"loop",      required_argument,  NULL, 'l'},
  {"output",    required_argument,  NULL, 'o'},
  {"dram",      required_argument,  NULL, 'd'},
  {"hot-refs",  required_argument,  NULL, 'H'},
  {"cold-refs", required_argument,  NULL, 'c'},
  {"migrate",   required_argument,  NULL, 'm'},
  {"verbose",   required_argument,  NULL, 'v'},
  {"help",      no_argument,        NULL, 'h'},
  {"changes",   no_argument,        NULL, 'g'},
  {"version",   no_argument,        NULL, 'r'},
  {"bitmap-max-pages", required_argument, NULL, 1007},
  {"bitmap-threads", required_argument, NULL, 1006},
  {"bitmap-batch-bytes", required_argument, NULL, 1005},
  {"scan",      required_argument,  NULL, 1000},
  {"backend",   required_argument,  NULL, 1001},
  {"addresses", required_argument,  NULL, 1002},
  {"ram-range", required_argument,  NULL, 1003},
  {"max-pageout-pages", required_argument, NULL, 1004},
  {NULL,        0,                  NULL, 0}
};

static void usage(char *prog)
{
  fprintf(stderr,
          "%s [option] ...\n"
          "    -h|--help       Show this information\n"
          "    -p|--pid        The PID to scan\n"
          "    -i|--interval   The scan interval in seconds\n"
          "    -l|--loop       The number of times to scan\n"
          "    -o|--output     The output file, defaults to refs-count-PID\n"
          "    -d|--dram       The DRAM percent, wrt. DRAM+PMEM total size\n"
          "    -H|--hot-refs   min_refs threshold for hot pages\n"
          "    -c|--cold-refs  max_refs threshold for cold pages\n"
          "    -m|--migrate    Migrate what: 0|none, 1|hot, 2|cold, 3|both\n"
          "    -v|--verbose    Show debug info\n"
          "    -r|--version    Show version info\n",
          prog);

  fprintf(stderr,
      "    --scan proc-idle|idle-bitmap\n"
      "    --backend numa|none|zswap\n"
      "    --bitmap-max-pages N  Allowlist limit (1..16777216; default 262144 = 1 GiB)\n"
      "    --bitmap-threads N  Idle bitmap workers (1..64; default 1)\n"
      "    --bitmap-batch-bytes N  Max contiguous bitmap I/O bytes (8..4096, multiple of 8; default 8)\n"
      "    --addresses FILE  Explicit HVA allowlist for idle-bitmap\n"
      "    --ram-range HEX:HEX  Allowed guest RAM HVA interval\n"
      "    --max-pageout-pages N  Maximum pageout attempts (default 4096)\n"
      "idle-bitmap requires -l 1..255, -i > 0 and backend none or zswap.\n"
      "idle-bitmap writes per-round I/O timing and counts to <output>.bitmap-stats.tsv.\n"
      "zswap additionally requires -c >= 0; pageout success is not a zswap guarantee.\n");

  exit(0);
}

static void parse_cmdline(int argc, char *argv[])
{
  int options_index = 0;
	int opt = 0;
	const char *optstr = "hvrp:i:l:o:d:H:c:m:";

  while ((opt = getopt_long(argc, argv, optstr, opts, &options_index)) != EOF) {
    switch (opt) {
    case 1000:
      option.scan_backend = optarg;
      break;
    case 1001:
      option.migration_backend = optarg;
      break;
    case 1002:
      option.address_file = optarg;
      break;
    case 1003: {
      char extra;
      if (sscanf(optarg, "%lx:%lx%c", &option.ram_start, &option.ram_end, &extra) != 2) {
        fprintf(stderr, "invalid --ram-range\n"); exit(2);
      }
      break;
    }
    case 1007: {
      char *end;
      errno = 0;
      unsigned long pages = strtoul(optarg, &end, 10);
      if (errno || *end || optarg[0] < '0' || optarg[0] > '9' ||
          pages < 1 || pages > 16777216) {
        fprintf(stderr, "invalid --bitmap-max-pages\n"); exit(2);
      }
      option.bitmap_max_pages = pages;
      break;
    }
    case 1006: {
      char *end;
      errno = 0;
      unsigned long threads = strtoul(optarg, &end, 10);
      if (errno || *end || optarg[0] < '0' || optarg[0] > '9' ||
          threads < 1 || threads > 64) {
        fprintf(stderr, "invalid --bitmap-threads\n"); exit(2);
      }
      option.bitmap_threads = threads;
      break;
    }
    case 1005: {
      char *end;
      errno = 0;
      unsigned long bytes = strtoul(optarg, &end, 10);
      if (errno || *end || optarg[0] < '0' || optarg[0] > '9' ||
          bytes < 8 || bytes > 4096 || bytes % 8) {
        fprintf(stderr, "invalid --bitmap-batch-bytes\n"); exit(2);
      }
      option.bitmap_batch_bytes = bytes;
      break;
    }
    case 1004: {
      char *end;
      errno = 0;
      option.max_pageout_pages = strtoul(optarg, &end, 10);
      if (errno || *end || !option.max_pageout_pages || optarg[0] < '0' || optarg[0] > '9') {
        fprintf(stderr, "invalid --max-pageout-pages\n"); exit(2);
      }
      break;
    }
    case 0:
      break;
    case 'p':
      option.pid = atoi(optarg);
      break;
    case 'i':
      option.interval = atof(optarg);
      break;
    case 'l':
      option.nr_walks = atoi(optarg);
      break;
    case 'o':
      option.output_file = optarg;
      break;
    case 'd':
      option.dram_percent = atoi(optarg);
      break;
    case 'H':
      option.hot_min_refs = atoi(optarg);
      break;
    case 'c':
      option.cold_max_refs = atoi(optarg);
      break;
    case 'm':
      option.migrate_what = Option::parse_migrate_name(optarg);
      break;
    case 'v':
      ++option.debug_level;
      break;
    case 'r':
      print_version();
      exit(0);
    case 'h':
    case '?':
    default:
      usage(argv[0]);
    }
  }

  if (!option.pid)
    usage(argv[0]);

  if ((option.scan_backend != "proc-idle" && option.scan_backend != "idle-bitmap") ||
      (option.migration_backend != "numa" && option.migration_backend != "none" && option.migration_backend != "zswap") ||
      (option.scan_backend == "idle-bitmap" && (option.migration_backend == "numa" ||
        option.address_file.empty() || option.nr_walks < 1 || option.nr_walks > 255)) ||
      (option.migration_backend == "zswap" && (option.scan_backend != "idle-bitmap" || option.cold_max_refs < 0))) {
    fprintf(stderr, "invalid scan/backend combination or missing POC options\n");
    exit(2);
  }

  if (option.output_file.empty())
    option.output_file = "refs-count-" + std::to_string(option.pid);
}

int account_refs(EPTMigrate& migration)
{
  int err;

  err = migration.walk_multi(option.nr_walks, option.interval);
  if (err)
    return err;

  EPTScan::reset_sys_refs_count(migration.get_nr_walks());
  migration.count_refs();

  err = migration.save_counts(option.output_file);
  if (err)
    return err;

  return 0;
}

int migrate(EPTMigrate& migration)
{
  int err = 0;

  err = migration.migrate();

  return err;
}

int main(int argc, char *argv[])
{
  int err = 0;

  setlocale(LC_NUMERIC, "");

  parse_cmdline(argc, argv);

  EPTMigrate migration;

  migration.set_pid(option.pid);

  err = account_refs(migration);
  if (err) {
    cout << "return err " << err;
	  return err;
  }

  err = migrate(migration);

  return err;
}
