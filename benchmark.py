#!/usr/bin/env python3
"""VeloxDB benchmarking suite.

Two independent measurements:

1. Index sweep - runs the C++ engine's in-process microbenchmark across a range
   of record counts, comparing hash and B-Tree lookups against flat array
   storage (linear scan and sorted array + binary search).

2. Head-to-head - runs an identical insert + point-lookup workload against
   VeloxDB and against a live MySQL server, reporting latency and throughput
   for both. Requires a reachable MySQL instance; skipped with a clear notice
   if one is not configured.

Every number printed is measured at runtime. Nothing here is hard-coded.
"""

import argparse
import json
import os
import platform
import subprocess
import sys
import time

DEFAULT_SWEEP = [1000, 10000, 100000]
KEY_PREFIX = "user_id_"
VALUE_PREFIX = "Payload_Record_Data_Block_"
PROBE_STRIDE = 7919  # coprime with the record counts used, so probes are strided
MYSQL_TABLE = "velox_bench"


# --------------------------------------------------------------------------
# VeloxDB engine driver
# --------------------------------------------------------------------------

def locate_engine(explicit=None):
    if explicit:
        if not os.path.exists(explicit):
            sys.exit("[FATAL] Engine not found at %s" % explicit)
        return explicit

    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("veloxdb_core.exe", "veloxdb_core"):
        candidate = os.path.join(here, name)
        if os.path.exists(candidate):
            return candidate

    sys.exit(
        "[FATAL] Compiled engine not found.\n"
        "        Build it first: g++ -O3 -std=c++14 veloxdb_core.cpp -o veloxdb_core"
    )


def run_engine(engine, args):
    result = subprocess.run(
        [engine] + args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        cwd=os.path.dirname(os.path.abspath(engine)),
    )
    if result.returncode != 0:
        sys.exit("[FATAL] Engine exited with code %d\n%s" % (result.returncode, result.stderr))
    return result.stdout


def parse_metrics(output, tag):
    """Pulls '[TAG] key=value' lines out of the engine's output."""
    metrics = {}
    prefix = "[%s]" % tag
    for line in output.splitlines():
        line = line.strip()
        if not line.startswith(prefix) or "=" not in line:
            continue
        key, _, value = line[len(prefix):].strip().partition("=")
        try:
            metrics[key.strip()] = float(value)
        except ValueError:
            metrics[key.strip()] = value.strip()
    return metrics


# --------------------------------------------------------------------------
# MySQL driver (optional dependency, resolved at runtime)
# --------------------------------------------------------------------------

def load_mysql_driver():
    try:
        import pymysql
        return "pymysql", pymysql
    except ImportError:
        pass
    try:
        import mysql.connector
        return "mysql-connector-python", mysql.connector
    except ImportError:
        pass
    return None, None


def mysql_connect(driver_name, driver, cfg):
    if driver_name == "pymysql":
        return driver.connect(
            host=cfg["host"], port=cfg["port"], user=cfg["user"],
            password=cfg["password"], database=cfg["database"], autocommit=True,
        )
    return driver.connect(
        host=cfg["host"], port=cfg["port"], user=cfg["user"],
        password=cfg["password"], database=cfg["database"], autocommit=True,
    )


def benchmark_mysql(driver_name, driver, cfg, records):
    """Same workload the engine runs: N single-row inserts, then N PK lookups."""
    conn = mysql_connect(driver_name, driver, cfg)
    cursor = conn.cursor()

    cursor.execute("DROP TABLE IF EXISTS %s" % MYSQL_TABLE)
    cursor.execute(
        "CREATE TABLE %s ("
        "  k VARCHAR(64) NOT NULL PRIMARY KEY,"
        "  v VARCHAR(255) NOT NULL"
        ") ENGINE=InnoDB" % MYSQL_TABLE
    )

    insert_sql = "INSERT INTO %s (k, v) VALUES (%%s, %%s)" % MYSQL_TABLE
    select_sql = "SELECT v FROM %s WHERE k = %%s" % MYSQL_TABLE

    start = time.perf_counter()
    for i in range(records):
        cursor.execute(insert_sql, ("%s%d" % (KEY_PREFIX, i), "%s%d" % (VALUE_PREFIX, i)))
    insert_seconds = time.perf_counter() - start

    hits = 0
    start = time.perf_counter()
    for i in range(records):
        cursor.execute(select_sql, ("%s%d" % (KEY_PREFIX, (i * PROBE_STRIDE) % records),))
        if cursor.fetchone():
            hits += 1
    select_seconds = time.perf_counter() - start

    cursor.execute("DROP TABLE IF EXISTS %s" % MYSQL_TABLE)
    cursor.close()
    conn.close()

    return {
        "records": records,
        "insert_total_s": insert_seconds,
        "insert_avg_us": (insert_seconds / records) * 1e6,
        "insert_ops_per_sec": records / insert_seconds,
        "select_total_s": select_seconds,
        "select_avg_us": (select_seconds / records) * 1e6,
        "select_ops_per_sec": records / select_seconds,
        "select_hits": hits,
    }


def mysql_version(driver_name, driver, cfg):
    conn = mysql_connect(driver_name, driver, cfg)
    cursor = conn.cursor()
    cursor.execute("SELECT VERSION()")
    version = cursor.fetchone()[0]
    cursor.close()
    conn.close()
    return version


# --------------------------------------------------------------------------
# Reporting
# --------------------------------------------------------------------------

def rule(char="-", width=78):
    print(char * width)


def header(title):
    print()
    rule("=")
    print(" " + title)
    rule("=")


def index_sweep(engine, sizes):
    header("1. INDEX SWEEP - lookup latency vs flat array storage")
    print(" Each row: one in-process run over N records, identical probe sequence")
    print(" for every structure. Latencies are nanoseconds per single lookup.")
    print()
    print(" %9s %10s %10s %10s %11s %12s" %
          ("records", "hash", "b-tree", "sorted[]", "linear[]", "vs linear"))
    rule()

    rows = []
    for size in sizes:
        m = parse_metrics(run_engine(engine, ["--bench", str(size)]), "BENCH")
        rows.append(m)
        print(" %9d %9.1fns %9.1fns %9.1fns %10.1fns %11.1fx" % (
            int(m["records"]), m["hash_lookup_ns"], m["btree_lookup_ns"],
            m["sorted_array_lookup_ns"], m["linear_array_lookup_ns"],
            m["speedup_vs_linear_x"],
        ))
    rule()

    for m in rows:
        print(" N=%-7d B-Tree height %d | hash load factor %.2f | indexed lookup is "
              "%.2f%% faster than linear scan, %.2f%% faster than binary search" % (
                  int(m["records"]), int(m["btree_height"]), m["hash_load_factor"],
                  m["faster_than_linear_pct"], m["faster_than_sorted_pct"]))
    return rows


def head_to_head(engine, records, mysql_cfg, skip_mysql):
    header("2. HEAD-TO-HEAD - VeloxDB vs MySQL, identical workload")
    print(" Workload: %d single-row inserts, then %d point lookups by key." % (records, records))
    print(" VeloxDB durability: write-ahead log flushed per mutation.")
    print(" MySQL durability:   InnoDB, autocommit on (one transaction per insert).")
    print()

    velox = parse_metrics(run_engine(engine, ["--workload", str(records)]), "WORKLOAD")

    mysql_result = None
    if skip_mysql:
        note = "skipped (--skip-mysql)"
    else:
        driver_name, driver = load_mysql_driver()
        if driver is None:
            note = ("no MySQL driver installed - run 'pip install pymysql' "
                    "to enable this comparison")
        else:
            try:
                version = mysql_version(driver_name, driver, mysql_cfg)
                mysql_result = benchmark_mysql(driver_name, driver, mysql_cfg, records)
                note = "MySQL %s via %s" % (version, driver_name)
            except Exception as exc:  # connection refused, auth failure, missing schema
                note = "MySQL unreachable at %s:%d - %s" % (
                    mysql_cfg["host"], mysql_cfg["port"], str(exc).split("\n")[0][:90])

    print(" %-22s %14s %14s %14s %14s" %
          ("engine", "insert us/op", "insert ops/s", "select us/op", "select ops/s"))
    rule()
    print(" %-22s %14.3f %14.0f %14.3f %14.0f" % (
        "VeloxDB", velox["insert_avg_us"], velox["insert_ops_per_sec"],
        velox["select_avg_us"], velox["select_ops_per_sec"]))

    if mysql_result:
        print(" %-22s %14.3f %14.0f %14.3f %14.0f" % (
            "MySQL (InnoDB)", mysql_result["insert_avg_us"], mysql_result["insert_ops_per_sec"],
            mysql_result["select_avg_us"], mysql_result["select_ops_per_sec"]))
        rule()
        for label in ("insert", "select"):
            ratio = mysql_result["%s_avg_us" % label] / velox["%s_avg_us" % label]
            if ratio >= 1.0:
                print(" VeloxDB %ss are %.1fx faster than MySQL" % (label, ratio))
            else:
                print(" VeloxDB %ss are %.1fx SLOWER than MySQL" % (label, 1.0 / ratio))
        print()
        print(" Caveat: MySQL pays for SQL parsing, a client/server round trip, and")
        print(" full ACID durability. This measures the cost of those guarantees, not")
        print(" a defect in MySQL.")
    else:
        rule()
        print(" MySQL side not measured: %s" % note)

    return velox, mysql_result, note


def main():
    parser = argparse.ArgumentParser(description="VeloxDB benchmarking suite")
    parser.add_argument("--engine", help="path to the compiled engine binary")
    parser.add_argument("--records", type=int, default=20000,
                        help="records for the head-to-head workload (default: 20000)")
    parser.add_argument("--sweep", type=int, nargs="+", default=DEFAULT_SWEEP,
                        help="record counts for the index sweep")
    parser.add_argument("--mysql-host", default="127.0.0.1")
    parser.add_argument("--mysql-port", type=int, default=3306)
    parser.add_argument("--mysql-user", default="root")
    parser.add_argument("--mysql-password", default=os.environ.get("MYSQL_PASSWORD", ""))
    parser.add_argument("--mysql-database", default="veloxdb_bench")
    parser.add_argument("--skip-mysql", action="store_true")
    parser.add_argument("--json", help="write raw results to this file")
    args = parser.parse_args()

    engine = locate_engine(args.engine)

    rule("=")
    print(" VELOXDB BENCHMARKING SUITE")
    rule("=")
    print(" engine   : %s" % engine)
    print(" platform : %s %s, Python %s" % (
        platform.system(), platform.machine(), platform.python_version()))

    sweep_rows = index_sweep(engine, args.sweep)

    mysql_cfg = {
        "host": args.mysql_host, "port": args.mysql_port, "user": args.mysql_user,
        "password": args.mysql_password, "database": args.mysql_database,
    }
    velox, mysql_result, note = head_to_head(engine, args.records, mysql_cfg, args.skip_mysql)

    if args.json:
        with open(args.json, "w") as handle:
            json.dump({
                "platform": platform.platform(),
                "index_sweep": sweep_rows,
                "veloxdb_workload": velox,
                "mysql_workload": mysql_result,
                "mysql_note": note,
            }, handle, indent=2)
        print("\n Raw results written to %s" % args.json)

    print()


if __name__ == "__main__":
    main()
