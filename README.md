# benchmark-matrix

![Language](https://img.shields.io/badge/language-C11-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)

`benchmark-matrix` is a C11 executor designed to run declared benchmark matrices and generate raw JSON artifacts. As a core module of the `benchmark-framework` library, it handles orchestration, process isolation, and metric collection.

## Features & Guarantees

The utility ensures that every workload profile is executed in strict isolation. Each run (both Single-Threaded and Multi-Threaded) is spawned in a clean child process (via `fork` and `exec`), completely eliminating shared state interference between iterations.

The matrix core guarantees:
* **Isolation and Safety**: Captures `stdout`/`stderr` (up to 1 MB per process) and prevents process hangs using a wall-clock timeout with forced termination (`SIGKILL`).
* **Protocol Validation**: Strictly enforces `benchmark-core` completion markers. It requires exactly one `benchmark=...` line followed immediately by a `Benchmark finished.` line.
* **Determinism**: Collects host metadata (OS, logical CPU count, scheduler affinity) and preserves all data generation parameters (seed, warmup, data count) to ensure 100% reproducible runs.

## Manifest and JSON Artifact

### Input
The utility accepts a versioned JSON manifest (`schema_version: 1`) containing an array of `profiles`. Each profile defines a unique `id` and specific parameters for the adapter: `input_kind`, `operation_kind`, `measure_mode`, `size_profile`, and `capacity_profile`.

### Output
The result is a single, atomic JSON artifact that includes:
1. **Environment & Config**: Host metadata (`host`) and execution parameters (`configuration`).
2. **Profiles**: The original input workload dimensions (`profiles`).
3. **Samples**: An array of execution results (`samples`). 
   - *Success*: Contains `elapsed_seconds` and `ns_per_call`.
   - *Failure*: If a process crashes or violates the protocol, the sample retains the `returncode`, captured `stdout`, and diagnostics in a `protocol_error` field.

## Command-Line Interface (CLI)

The utility requires explicit file paths for its core operations:
`--manifest <FILE>`, `--output <FILE>`, `--st-binary <FILE>`, and `--mt-binary <FILE>`.

It also supports the following configuration parameters (all of which have deterministic defaults):
* `--repetitions` — Number of independent runs for each profile and mode (ST/MT).
* `--iterations` — Number of iterations for ST mode.
* `--mt-total-iterations` — Total number of iterations for MT mode (must be evenly divisible by the number of threads).
* `--threads` — Number of MT workers.
* `--warmup`, `--data-count`, `--seed` — Data generation and warm-up parameters forwarded to the child processes.
* `--timeout-seconds` — Wall-clock time limit for a single child process.

### Example Usage

```bash
./bench_matrix \
  --manifest profiles.json \
  --output results.json \
  --st-binary ./my_bench_st \
  --mt-binary ./my_bench_mt \
  --threads 4 \
  --repetitions 5
```

## Testing & Validation

To ensure reliability and memory safety, the project includes a comprehensive test suite.

```bash
make test
make lint
make test_sanitize
make clean && make test_helgrind
```

* **`make test`**: Verifies JSON manifest parsing, CLI error handling, child process output capture, timeout triggers, and the final JSON artifact formatting.
* **`make test_sanitize`**: Runs the suite with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan).
* **`make test_helgrind`**: Checks the MT lifecycle for data races using Valgrind's Helgrind tool.

## License

This project is part of the `benchmark-framework` and is distributed under the terms of the project's primary license. See the `LICENSE` file in the root directory for more details.
