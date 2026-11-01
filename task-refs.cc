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

#include <cerrno>
#include <cctype>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/types.h>

#include "Option.h"
#include "ProcMaps.h"
#include "ProcIdlePages.h"
#include "EPTScan.h"
#include "EPTMigrate.h"
#include "IdleBitmapScan.h"
#include "IdleSnapshot.h"
#include "lib/debug.h"
#include "version.h"

Option option;

struct IdleBitmapOptions
{
  bool enabled = false;
  uint64_t start = 0;
  uint64_t end = 0;
  unsigned window_ms = 100;
  bool range_specified = false;
  bool window_specified = false;
  bool legacy_options_specified = false;
};

static IdleBitmapOptions idle_options;

enum {
  OPT_SCAN = 1000,
  OPT_RAM_RANGE,
  OPT_WINDOW_MS
};

static uint64_t parse_unsigned(const std::string& text, int base)
{
  char* end = nullptr;

  if (text.empty() || text[0] == '-' || text[0] == '+' ||
      std::isspace(static_cast<unsigned char>(text[0])))
    throw std::invalid_argument("invalid unsigned number: " + text);

  errno = 0;
  const uint64_t value = std::strtoull(text.c_str(), &end, base);

  if (errno != 0 || end == text.c_str() || *end != '\0')
    throw std::invalid_argument("invalid unsigned number: " + text);

  return value;
}

static uint64_t parse_address(const std::string& text)
{
  /* Only an explicit hexadecimal prefix changes the default decimal base. */
  const bool hexadecimal = text.size() >= 2 && text[0] == '0' &&
                           (text[1] == 'x' || text[1] == 'X');
  return parse_unsigned(text, hexadecimal ? 16 : 10);
}

int debug_level()
{
  return option.debug_level;
}

static const struct option opts[] = {
  {"scan",      required_argument,  NULL, OPT_SCAN},
  {"ram-range", required_argument,  NULL, OPT_RAM_RANGE},
  {"window-ms", required_argument,  NULL, OPT_WINDOW_MS},
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
          "Idle bitmap snapshot (does not migrate pages):\n"
          "    --scan idle-bitmap --pid PID --ram-range START:END "
          "--output FILE\n"
          "    --window-ms N   Wait after marking idle, default 100 ms\n"
          "    START/END       Page-aligned HVA, decimal or 0x/0X hex; "
          "end excluded\n");

  exit(0);
}

static void parse_cmdline(int argc, char *argv[])
{
  int options_index = 0;
  int opt = 0;
  const char *optstr = "hvrp:i:l:o:d:H:c:m:";

  while ((opt = getopt_long(argc, argv, optstr, opts, &options_index)) != EOF) {
    switch (opt) {
    case OPT_SCAN:
      if (std::string(optarg) != "idle-bitmap")
        throw std::invalid_argument("supported --scan value: idle-bitmap");
      idle_options.enabled = true;
      break;
    case OPT_RAM_RANGE: {
      const std::string range(optarg);
      const auto colon = range.find(':');

      if (colon == std::string::npos)
        throw std::invalid_argument("--ram-range requires START:END");

      idle_options.start = parse_address(range.substr(0, colon));
      idle_options.end = parse_address(range.substr(colon + 1));
      idle_options.range_specified = true;
      break;
    }
    case OPT_WINDOW_MS: {
      const uint64_t value = parse_unsigned(optarg, 10);

      if (value == 0 || value > static_cast<uint64_t>(UINT_MAX))
        throw std::invalid_argument("--window-ms is out of range");

      idle_options.window_ms = static_cast<unsigned>(value);
      idle_options.window_specified = true;
      break;
    }
    case 0:
      break;
    case 'p': {
      const uint64_t pid = parse_unsigned(optarg, 10);

      if (pid == 0 || pid > static_cast<uint64_t>(INT_MAX))
        throw std::invalid_argument("PID is out of range");

      option.pid = static_cast<int>(pid);
      break;
    }
    case 'i':
      idle_options.legacy_options_specified = true;
      option.interval = atof(optarg);
      break;
    case 'l':
      idle_options.legacy_options_specified = true;
      option.nr_walks = atoi(optarg);
      break;
    case 'o':
      option.output_file = optarg;
      break;
    case 'd':
      idle_options.legacy_options_specified = true;
      option.dram_percent = atoi(optarg);
      break;
    case 'H':
      idle_options.legacy_options_specified = true;
      option.hot_min_refs = atoi(optarg);
      break;
    case 'c':
      idle_options.legacy_options_specified = true;
      option.cold_max_refs = atoi(optarg);
      break;
    case 'm':
      idle_options.legacy_options_specified = true;
      option.migrate_what = Option::parse_migrate_name(optarg);
      break;
    case 'v':
      ++option.debug_level;
      break;
    case 'r':
      print_version();
      exit(0);
    case 'h':
      usage(argv[0]);
      break;
    case '?':
    default:
      throw std::invalid_argument(
          "unrecognized or incomplete option; see --help");
    }
  }

  if (idle_options.enabled) {
    if (option.pid <= 0 || !idle_options.range_specified ||
        option.output_file.empty() || idle_options.start >= idle_options.end ||
        idle_options.start % IDLE_BITMAP_PAGE_SIZE != 0 ||
        idle_options.end % IDLE_BITMAP_PAGE_SIZE != 0)
      throw std::invalid_argument(
          "idle-bitmap requires positive PID, aligned range and output file");

    if (idle_options.legacy_options_specified)
      throw std::invalid_argument(
          "idle-bitmap uses --window-ms; "
          "legacy scan/migration options do not apply");
  } else if (idle_options.range_specified || idle_options.window_specified) {
    throw std::invalid_argument(
        "--ram-range/--window-ms require --scan idle-bitmap");
  }

  if (optind != argc)
    throw std::invalid_argument("unexpected positional argument");

  if (!option.pid)
    usage(argv[0]);

  if (option.output_file.empty())
    option.output_file = "refs-count-" + std::to_string(option.pid);
}

static int save_idle_snapshot()
{
  const IdleSample sample = sample_idle_pages(
      option.pid, idle_options.start, idle_options.end, idle_options.window_ms);

  write_idle_snapshot(sample, option.output_file);

  size_t cold = 0;
  size_t hot = 0;
  size_t invalid = 0;

  for (const IdlePage& page : sample.pages) {
    if (!page.observed)
      ++invalid;
    else if (page.refs != 0)
      ++hot;
    else
      ++cold;
  }

  fprintf(stderr,
          "idle-bitmap: cold=%zu hot=%zu invalid=%zu "
          "clear_start_ns=%" PRIu64 " clear_end_ns=%" PRIu64 " "
          "check_start_ns=%" PRIu64 " check_end_ns=%" PRIu64 "\n",
          cold, hot, invalid,
          sample.clear_start_ns, sample.clear_end_ns,
          sample.check_start_ns, sample.check_end_ns);

  return 0;
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

  try {
    parse_cmdline(argc, argv);

    if (idle_options.enabled)
      return save_idle_snapshot();
  } catch (const std::exception& error) {
    std::cerr << "task-refs: " << error.what() << std::endl;
    return 1;
  }

  EPTMigrate migration;

  migration.set_pid(option.pid);

  err = account_refs(migration);
  if (err) {
    std::cout << "return err " << err;
    return err;
  }

  err = migrate(migration);

  return err;
}
