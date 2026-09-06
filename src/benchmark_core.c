/**
 * @file benchmark_core.c
 * @brief Реализация generic C11 benchmark lifecycle для project-owned adapters.
 * @details
 * Core проводит deterministic data generation, warm-up, measurement и protocol
 * emission. Для in-place callbacks source dataset всегда immutable; ST использует
 * reusable workspace, MT создаёт per-worker state и coordinator barrier. Timing
 * выбирает end-to-end или kernel-only boundary без навязывания domain semantics.
 */
#define _POSIX_C_SOURCE 200809L

#include "benchmark_core.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief Выбирает timing boundary измеряемого lifecycle. */
typedef enum {
    MEASURE_END_TO_END = 0, /**< Include source-to-mutable preparation in timing. */
    MEASURE_KERNEL_ONLY = 1 /**< Exclude preparation copy from timed interval. */
} measure_mode_t;

/**
 * @brief Хранит нормализованные CLI/ENV options одного ST или MT run.
 * @details Алгоритм parsing создаёт defaults, применяет ENV как baseline и затем
 * explicit CLI override. custom_profile отражает переход legacy data-mode к
 * независимым workload dimensions без потери backward compatibility.
 */
typedef struct {
    benchmark_workload_t workload; /**< Immutable callback metadata after CLI/ENV normalization. */
    measure_mode_t measure_mode; /**< Parsed timing-boundary enum corresponding to workload.measure_mode. */
    uint64_t iterations; /**< ST measured operation count. */
    uint64_t total_iterations; /**< MT aggregate measured operation count. */
    size_t threads; /**< MT worker count; a positive divisor of total_iterations. */
    benchmark_boolean_t custom_profile; /**< True when independent axes replace legacy data-mode preset. */
} benchmark_options_t;

/**
 * @brief Владеет immutable contiguous source-state dataset.
 * @details sources содержит count records фиксированного state_size. fingerprint
 * вычисляется после initialize callbacks и печатается в protocol для проверки
 * воспроизводимости input data между reviewed baseline и candidate.
 */
typedef struct {
    unsigned char *sources; /**< Owned contiguous immutable source-state allocation. */
    size_t count; /**< Number of source records available to cyclic iteration. */
    size_t state_size; /**< Byte stride of one source record. */
    uint64_t fingerprint; /**< Deterministic FNV-style summary of initialized source bytes. */
} benchmark_dataset_t;

/**
 * @brief Содержит один MT worker context и barrier coordination references.
 * @details Каждый worker получает уникальный worker_id и private checksum/success/
 * elapsed fields. Shared mutex/condition управляют только ready/released/aborted,
 * поэтому adapter state не разделяется между pthreads.
 */
typedef struct {
    const benchmark_dataset_t *dataset; /**< Shared immutable source dataset; never mutated by worker. */
    const benchmark_options_t *options; /**< Shared immutable normalized run configuration. */
    const benchmark_adapter_t *adapter; /**< Shared immutable callback binding. */
    size_t worker_id; /**< Stable zero-based worker index and source-row offset. */
    pthread_mutex_t *mutex; /**< Coordinator-owned mutex protecting barrier flags/counter. */
    pthread_cond_t *condition; /**< Coordinator/worker condition variable for readiness and release. */
    size_t *ready_workers; /**< Shared count of workers that completed warm-up. */
    benchmark_boolean_t *released; /**< Shared true flag permitting measured execution. */
    benchmark_boolean_t *aborted; /**< Shared true flag requesting early worker exit. */
    uint64_t checksum; /**< Private post-operation checksum reduction read after join. */
    uint64_t successful; /**< Private successful operation count read after join. */
    double kernel_elapsed_seconds; /**< Private accumulated kernel-only duration in seconds. */
    benchmark_boolean_t failed; /**< Private callback/allocation failure indicator read after join. */
} benchmark_worker_t;

#define DEFAULT_ST_ITERATIONS UINT64_C(2000000000)
#define DEFAULT_MT_TOTAL_ITERATIONS UINT64_C(3200000000)
#define DEFAULT_WARMUP UINT64_C(10000)
#define DEFAULT_DATA_COUNT 4096U
#define DEFAULT_THREADS 2U
#define DEFAULT_SEED UINT64_C(0x9E3779B97F4A7C15)

/** @brief Разбирает unsigned integer в decimal или hexadecimal notation.
 * @details Алгоритм использует strtoull base 0, проверяет errno/end pointer и
 * записывает output только после полного валидного consumption text.
 */
static benchmark_core_status_t parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    *value = (uint64_t)parsed;
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Разбирает строго положительный size_t с overflow guard.
 * @details Алгоритм делегирует parse_u64, отвергает zero и сравнивает с SIZE_MAX
 * до narrowing conversion, защищая dataset/thread allocation sizes.
 */
static benchmark_core_status_t parse_size(const char *text, size_t *value)
{
    uint64_t parsed;

    if (parse_u64(text, &parsed) != BENCHMARK_CORE_STATUS_SUCCESS || parsed == 0U || parsed > SIZE_MAX) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    *value = (size_t)parsed;
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Преобразует declared measure-mode text в internal enum.
 * @details Алгоритм принимает ровно end-to-end или kernel-only, не оставляя
 * неизвестному token возможности изменить timing boundary неявно.
 */
static benchmark_core_status_t parse_measure(const char *text, measure_mode_t *value)
{
    if (strcmp(text, "end-to-end") == 0) *value = MEASURE_END_TO_END;
    else if (strcmp(text, "kernel-only") == 0) *value = MEASURE_KERNEL_ONLY;
    else return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Возвращает canonical protocol string internal measure enum.
 * @details Алгоритм является total mapping двух enum values в stable JSON/protocol text.
 */
static const char *measure_name(measure_mode_t value)
{
    return value == MEASURE_KERNEL_ONLY ? "kernel-only" : "end-to-end";
}

/** @brief Проверяет safe single-token workload/benchmark identifier.
 * @details Алгоритм отвергает NULL, empty strings, whitespace и equal sign, чтобы
 * machine-readable key=value protocol и adapter argument processing оставались
 * однозначными без quoting semantics.
 */
static benchmark_boolean_t valid_token(const char *value)
{
    return value != NULL && *value != '\0' && strpbrk(value, " \t\r\n=") == NULL
        ? BENCHMARK_BOOLEAN_TRUE : BENCHMARK_BOOLEAN_FALSE;
}

/** @brief Отображает legacy data-mode в независимые workload dimensions.
 * @details Алгоритм задаёт historical all_zero/all_nonzero/mixed presets и
 * очищает custom_profile. Subsequent explicit axis options могут намеренно
 * переключить data_mode в custom, сохраняя обратную CLI совместимость.
 */
static benchmark_core_status_t apply_legacy_mode(const char *text, benchmark_options_t *options)
{
    if (strcmp(text, "all_zero") == 0) {
        options->workload.input_kind = "zero";
        options->workload.operation_kind = "noop";
        options->workload.size_profile = "tiny";
    } else if (strcmp(text, "all_nonzero") == 0) {
        options->workload.input_kind = "nonzero";
        options->workload.operation_kind = "default";
        options->workload.size_profile = "medium";
    } else if (strcmp(text, "mixed") == 0) {
        options->workload.input_kind = "mixed";
        options->workload.operation_kind = "mixed";
        options->workload.size_profile = "variable";
    } else {
        return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    }
    options->workload.data_mode = text;
    options->custom_profile = BENCHMARK_BOOLEAN_FALSE;
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Применяет optional BENCH_* environment overrides к defaults.
 * @details Алгоритм валидирует все numeric/token values до их назначения,
 * различает ST/MT iteration variables и отмечает custom profile при независимых
 * workload axes. CLI parsing вызывается позднее и имеет преднамеренный приоритет.
 */
static benchmark_core_status_t apply_environment(benchmark_options_t *options, benchmark_boolean_t multithreaded)
{
    const char *text;

    if (multithreaded == BENCHMARK_BOOLEAN_FALSE && (text = getenv("BENCH_ITERATIONS")) != NULL &&
        (parse_u64(text, &options->iterations) != BENCHMARK_CORE_STATUS_SUCCESS || options->iterations == 0U)) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if (multithreaded == BENCHMARK_BOOLEAN_TRUE && (text = getenv("BENCH_MT_TOTAL_ITERATIONS")) != NULL &&
        (parse_u64(text, &options->total_iterations) != BENCHMARK_CORE_STATUS_SUCCESS || options->total_iterations == 0U)) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if (multithreaded == BENCHMARK_BOOLEAN_TRUE && (text = getenv("BENCH_MT_THREADS")) != NULL &&
        parse_size(text, &options->threads) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_WARMUP")) != NULL && parse_u64(text, &options->workload.warmup) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_DATA_COUNT")) != NULL && parse_size(text, &options->workload.data_count) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_SEED")) != NULL && parse_u64(text, &options->workload.seed) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_INPUT_KIND")) != NULL && valid_token(text) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_OPERATION_KIND")) != NULL && valid_token(text) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_MEASURE_MODE")) != NULL && parse_measure(text, &options->measure_mode) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_SIZE_PROFILE")) != NULL && valid_token(text) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_CAPACITY_PROFILE")) != NULL && valid_token(text) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    if ((text = getenv("BENCH_INPUT_KIND")) != NULL) {
        options->workload.input_kind = text;
        options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
    }
    if ((text = getenv("BENCH_OPERATION_KIND")) != NULL) {
        options->workload.operation_kind = text;
        options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
    }
    if ((text = getenv("BENCH_MEASURE_MODE")) != NULL) options->workload.measure_mode = text;
    if ((text = getenv("BENCH_SIZE_PROFILE")) != NULL) {
        options->workload.size_profile = text;
        options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
    }
    if ((text = getenv("BENCH_CAPACITY_PROFILE")) != NULL) {
        options->workload.capacity_profile = text;
        options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
    }
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/**
 * @brief Нормализует CLI и ENV в benchmark options.
 * @details Алгоритм инициализирует canonical defaults, применяет environment,
 * обрабатывает option/value pairs, поддерживает --help success return и требует
 * MT iteration divisibility. При custom axes data_mode становится custom перед
 * тем, как структура будет передана lifecycle callbacks и protocol.
 */
static benchmark_core_status_t parse_options(int argc, char **argv, benchmark_boolean_t multithreaded, benchmark_options_t *options)
{
    *options = (benchmark_options_t){
        .workload = {
            .data_mode = "all_nonzero",
            .input_kind = "nonzero",
            .operation_kind = "default",
            .measure_mode = "end-to-end",
            .size_profile = "medium",
            .capacity_profile = "normal",
            .seed = DEFAULT_SEED,
            .warmup = DEFAULT_WARMUP,
            .data_count = DEFAULT_DATA_COUNT
        },
        .measure_mode = MEASURE_END_TO_END,
        .iterations = DEFAULT_ST_ITERATIONS,
        .total_iterations = DEFAULT_MT_TOTAL_ITERATIONS,
        .threads = DEFAULT_THREADS,
        .custom_profile = BENCHMARK_BOOLEAN_FALSE
    };
    if (apply_environment(options, multithreaded) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;

    for (int index = 1; index < argc; ++index) {
        const char *option = argv[index];
        const char *value;

        if (strcmp(option, "--help") == 0) {
            printf("usage: %s [--data-mode all_zero|all_nonzero|mixed] [--input-kind TOKEN] [--operation-kind TOKEN] [--measure-mode end-to-end|kernel-only] [--size-profile TOKEN] [--capacity-profile TOKEN] [--warmup N] [--data-count N] [--seed N]%s\n",
                argv[0], multithreaded ? " [--threads N] [--total-iterations N]" : " [--iterations N]");
            return BENCHMARK_CORE_STATUS_HELP;
        }
        if (index + 1 >= argc) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        value = argv[++index];
        if (strcmp(option, "--data-mode") == 0) {
            if (apply_legacy_mode(value, options) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        } else if (strcmp(option, "--input-kind") == 0) {
            if (valid_token(value) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
            options->workload.input_kind = value;
            options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
        } else if (strcmp(option, "--operation-kind") == 0) {
            if (valid_token(value) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
            options->workload.operation_kind = value;
            options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
        } else if (strcmp(option, "--measure-mode") == 0) {
            if (parse_measure(value, &options->measure_mode) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
            options->workload.measure_mode = value;
        } else if (strcmp(option, "--size-profile") == 0) {
            if (valid_token(value) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
            options->workload.size_profile = value;
            options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
        } else if (strcmp(option, "--capacity-profile") == 0) {
            if (valid_token(value) != BENCHMARK_BOOLEAN_TRUE) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
            options->workload.capacity_profile = value;
            options->custom_profile = BENCHMARK_BOOLEAN_TRUE;
        } else if (strcmp(option, "--warmup") == 0) {
            if (parse_u64(value, &options->workload.warmup) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        } else if (strcmp(option, "--data-count") == 0) {
            if (parse_size(value, &options->workload.data_count) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        } else if (strcmp(option, "--seed") == 0) {
            if (parse_u64(value, &options->workload.seed) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        } else if (!multithreaded && strcmp(option, "--iterations") == 0) {
            if (parse_u64(value, &options->iterations) != BENCHMARK_CORE_STATUS_SUCCESS || options->iterations == 0U) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        } else if (multithreaded && strcmp(option, "--threads") == 0) {
            if (parse_size(value, &options->threads) != BENCHMARK_CORE_STATUS_SUCCESS) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        } else if (multithreaded && strcmp(option, "--total-iterations") == 0) {
            if (parse_u64(value, &options->total_iterations) != BENCHMARK_CORE_STATUS_SUCCESS || options->total_iterations == 0U) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        } else {
            return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
        }
    }
    if (options->custom_profile == BENCHMARK_BOOLEAN_TRUE) options->workload.data_mode = "custom";
    if (multithreaded == BENCHMARK_BOOLEAN_TRUE && options->total_iterations % (uint64_t)options->threads != 0U) return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Проверяет полноту project-owned adapter binding.
 * @details Алгоритм требует safe benchmark name, nonzero state_size и все три
 * callbacks. Проверка выполняется до dataset allocation и pthread creation.
 */
static benchmark_boolean_t valid_adapter(const benchmark_adapter_t *adapter)
{
    return adapter != NULL && valid_token(adapter->benchmark_name) == BENCHMARK_BOOLEAN_TRUE &&
        adapter->state_size != 0U && adapter->initialize != NULL &&
        adapter->operation != NULL && adapter->checksum != NULL
        ? BENCHMARK_BOOLEAN_TRUE : BENCHMARK_BOOLEAN_FALSE;
}

/** @brief Возвращает pointer на state record заданного dataset index.
 * @details Алгоритм использует contiguous layout и state_size stride; caller
 * всегда передаёт index, полученный modulo dataset count.
 */
static void *dataset_item(const benchmark_dataset_t *dataset, size_t index)
{
    return dataset->sources + index * dataset->state_size;
}

/** @brief Смешивает bytes state record в 64-bit dataset fingerprint.
 * @details Алгоритм реализует FNV-style xor/multiply recurrence и вызывается
 * последовательно для каждого initialized record, создавая reproducibility signal.
 */
static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t index = 0U; index < size; ++index) {
        hash ^= (uint64_t)bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/** @brief Выделяет и детерминированно инициализирует immutable source dataset.
 * @details Алгоритм проверяет multiplication overflow, calloc-ит contiguous rows,
 * вызывает adapter initialize для stable sequence indexes и обновляет fingerprint.
 * Callback failure освобождает partial storage и возвращает error без leaked state.
 */
static benchmark_core_status_t dataset_create(benchmark_dataset_t *dataset, const benchmark_options_t *options,
    const benchmark_adapter_t *adapter)
{
    size_t allocation_size;
    uint64_t hash = UINT64_C(1469598103934665603);

    if (options->workload.data_count > SIZE_MAX / adapter->state_size) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    allocation_size = options->workload.data_count * adapter->state_size;
    *dataset = (benchmark_dataset_t){
        .count = options->workload.data_count,
        .state_size = adapter->state_size
    };
    dataset->sources = calloc(1U, allocation_size);
    if (dataset->sources == NULL) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    for (size_t index = 0U; index < dataset->count; ++index) {
        void *state = dataset_item(dataset, index);
        if (adapter->initialize(state, (uint64_t)index, &options->workload,
                adapter->adapter_context) != adapter->success_code) {
            free(dataset->sources);
            *dataset = (benchmark_dataset_t){0};
            return BENCHMARK_CORE_STATUS_CALLBACK_ERROR;
        }
        hash = hash_bytes(hash, state, dataset->state_size);
    }
    dataset->fingerprint = hash;
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Освобождает dataset storage и обнуляет ownership descriptor.
 * @details Алгоритм безопасен после partial dataset_create: free(NULL) допустим,
 * aggregate reset предотвращает accidental повторное использование stale layout.
 */
static void dataset_destroy(benchmark_dataset_t *dataset)
{
    free(dataset->sources);
    *dataset = (benchmark_dataset_t){0};
}

/** @brief Преобразует два monotonic timespec в duration seconds.
 * @details Алгоритм вычитает seconds/nanoseconds как double; CLOCK_MONOTONIC
 * callers исключают wall-clock adjustment из benchmark timing interval.
 */
static double seconds_between(const struct timespec *start, const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
        (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

/** @brief Вызывает adapter operation и нормализует success code в 0/-1.
 * @details Алгоритм передаёт immutable workload/context и переводит domain-specific
 * success_code в internal result, единый для ST, MT, warm-up и обеих timing границ.
 */
static benchmark_core_status_t invoke(const benchmark_adapter_t *adapter, void *state, uint64_t iteration,
    const benchmark_workload_t *workload)
{
    return adapter->operation(state, iteration, workload, adapter->adapter_context) ==
        adapter->success_code ? BENCHMARK_CORE_STATUS_SUCCESS : BENCHMARK_CORE_STATUS_CALLBACK_ERROR;
}

/** @brief Выполняет declared warm-up без включения его в measurement duration.
 * @details Алгоритм выделяет private workspace, циклически выбирает source rows,
 * memcpy-ит immutable state и вызывает operation. worker_offset разделяет starting
 * rows workers, не создавая shared mutable state.
 */
static benchmark_core_status_t warm_up(const benchmark_dataset_t *dataset, uint64_t count,
    const benchmark_options_t *options, const benchmark_adapter_t *adapter, size_t worker_offset)
{
    unsigned char *state = malloc(dataset->state_size);
    if (state == NULL) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    for (uint64_t iteration = 0U; iteration < count; ++iteration) {
        const size_t row = (size_t)((iteration + worker_offset) % dataset->count);
        memcpy(state, dataset_item(dataset, row), dataset->state_size);
        if (invoke(adapter, state, iteration, &options->workload) != BENCHMARK_CORE_STATUS_SUCCESS) {
            free(state);
            return BENCHMARK_CORE_STATUS_CALLBACK_ERROR;
        }
    }
    free(state);
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Измеряет ST operation вместе с preparation copy.
 * @details Алгоритм запускает CLOCK_MONOTONIC до первого memcpy, для каждой logical
 * iteration копирует source row, выполняет operation и смешивает checksum, затем
 * останавливает clock. Таким образом elapsed включает full mutable lifecycle.
 */
static benchmark_core_status_t execute_st_end_to_end(const benchmark_dataset_t *dataset, uint64_t iterations,
    const benchmark_options_t *options, const benchmark_adapter_t *adapter,
    double *elapsed, uint64_t *checksum)
{
    struct timespec start;
    struct timespec end;
    unsigned char *state = malloc(dataset->state_size);
    if (state == NULL) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        free(state);
        return BENCHMARK_CORE_STATUS_CLOCK_ERROR;
    }
    for (uint64_t iteration = 0U; iteration < iterations; ++iteration) {
        const size_t row = (size_t)(iteration % dataset->count);
        memcpy(state, dataset_item(dataset, row), dataset->state_size);
        if (invoke(adapter, state, iteration, &options->workload) != BENCHMARK_CORE_STATUS_SUCCESS) {
            free(state);
            return BENCHMARK_CORE_STATUS_CALLBACK_ERROR;
        }
        *checksum ^= adapter->checksum(state, iteration, adapter->adapter_context);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        free(state);
        return BENCHMARK_CORE_STATUS_CLOCK_ERROR;
    }
    free(state);
    *elapsed = seconds_between(&start, &end);
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Измеряет ST operation без source-to-workspace preparation copy.
 * @details Алгоритм делит iterations на dataset-sized batches, копирует batch до
 * clock start и измеряет только operation/checksum loop. Batch durations суммируются,
 * поэтому kernel-only не включает memcpy, но сохраняет identical mutable semantics.
 */
static benchmark_core_status_t execute_st_kernel_only(const benchmark_dataset_t *dataset, uint64_t iterations,
    const benchmark_options_t *options, const benchmark_adapter_t *adapter,
    double *elapsed, uint64_t *checksum)
{
    unsigned char *workspace;
    uint64_t completed = 0U;
    size_t allocation_size;

    if (dataset->count > SIZE_MAX / dataset->state_size) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    allocation_size = dataset->count * dataset->state_size;
    workspace = malloc(allocation_size);
    if (workspace == NULL) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    *elapsed = 0.0;
    while (completed < iterations) {
        const size_t batch = (size_t)((iterations - completed) < dataset->count
            ? (iterations - completed) : dataset->count);
        struct timespec start;
        struct timespec end;

        /* Copy occurs before the timer: kernel-only isolates operation from preparation cost. */
        memcpy(workspace, dataset->sources, batch * dataset->state_size);
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) { free(workspace); return BENCHMARK_CORE_STATUS_CLOCK_ERROR; }
        for (size_t row = 0U; row < batch; ++row) {
            void *state = workspace + row * dataset->state_size;
            if (invoke(adapter, state, completed + row, &options->workload) != BENCHMARK_CORE_STATUS_SUCCESS) {
                free(workspace);
                return BENCHMARK_CORE_STATUS_CALLBACK_ERROR;
            }
            *checksum ^= adapter->checksum(state, completed + row, adapter->adapter_context);
        }
        if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) { free(workspace); return BENCHMARK_CORE_STATUS_CLOCK_ERROR; }
        *elapsed += seconds_between(&start, &end);
        completed += batch;
    }
    free(workspace);
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Печатает обязательный ST machine-readable completion protocol.
 * @details Алгоритм вычисляет ns_per_call, emits exactly one benchmark= line со
 * всеми workload/fingerprint/checksum fields и затем Benchmark finished. marker;
 * порядок сохраняет Makefile и matrix-runner compatibility.
 */
static void emit_st(const benchmark_adapter_t *adapter, const benchmark_options_t *options,
    const benchmark_dataset_t *dataset, uint64_t successful, uint64_t checksum, double elapsed)
{
    const double ns_per_call = elapsed * 1000000000.0 / (double)options->iterations;
    printf("benchmark=%s_st data_mode=%s input_kind=%s operation_kind=%s measure_mode=%s size_profile=%s capacity_profile=%s seed=%" PRIu64 " warmup=%" PRIu64 " data_count=%zu iterations=%" PRIu64 " successful=%" PRIu64 " fingerprint=%" PRIu64 " checksum=%" PRIu64 " elapsed_seconds=%.9f ns_per_call=%.3f\n",
        adapter->benchmark_name, options->workload.data_mode, options->workload.input_kind,
        options->workload.operation_kind, measure_name(options->measure_mode),
        options->workload.size_profile, options->workload.capacity_profile,
        options->workload.seed, options->workload.warmup, options->workload.data_count,
        options->iterations, successful, dataset->fingerprint, checksum, elapsed, ns_per_call);
    puts("Benchmark finished.");
}

/** @brief Печатает обязательный aggregated MT completion protocol.
 * @details Алгоритм восстанавливает total iterations из per-thread value, вычисляет
 * aggregate ns_per_call и emits ordered benchmark= затем completion marker.
 */
static void emit_mt(const benchmark_adapter_t *adapter, const benchmark_options_t *options,
    const benchmark_dataset_t *dataset, uint64_t iterations_per_thread,
    uint64_t successful, uint64_t checksum, double elapsed)
{
    const uint64_t total_iterations = iterations_per_thread * (uint64_t)options->threads;
    const double ns_per_call = elapsed * 1000000000.0 / (double)total_iterations;
    printf("benchmark=%s_mt data_mode=%s input_kind=%s operation_kind=%s measure_mode=%s size_profile=%s capacity_profile=%s seed=%" PRIu64 " warmup=%" PRIu64 " data_count=%zu threads=%zu iterations_per_thread=%" PRIu64 " total_iterations=%" PRIu64 " successful=%" PRIu64 " fingerprint=%" PRIu64 " checksum=%" PRIu64 " elapsed_seconds=%.9f ns_per_call=%.3f\n",
        adapter->benchmark_name, options->workload.data_mode, options->workload.input_kind,
        options->workload.operation_kind, measure_name(options->measure_mode),
        options->workload.size_profile, options->workload.capacity_profile,
        options->workload.seed, options->workload.warmup, options->workload.data_count,
        options->threads, iterations_per_thread, total_iterations, successful,
        dataset->fingerprint, checksum, elapsed, ns_per_call);
    puts("Benchmark finished.");
}

/**
 * @brief Исполняет complete single-thread benchmark lifecycle.
 * @details Алгоритм parses options, validates adapter, creates dataset, runs warm-up,
 * dispatches selected measurement boundary, emits protocol only after success и
 * уничтожает dataset на всех success/failure paths.
 */
benchmark_core_status_t benchmark_core_run_st(int argc, char **argv, const benchmark_adapter_t *adapter)
{
    benchmark_options_t options;
    benchmark_dataset_t dataset;
    uint64_t checksum = 0U;
    double elapsed = 0.0;
    benchmark_core_status_t result;
    const benchmark_core_status_t parse_result = parse_options(
        argc, argv, BENCHMARK_BOOLEAN_FALSE, &options);

    if (parse_result == BENCHMARK_CORE_STATUS_HELP) return BENCHMARK_CORE_STATUS_SUCCESS;
    if (parse_result != BENCHMARK_CORE_STATUS_SUCCESS ||
        valid_adapter(adapter) != BENCHMARK_BOOLEAN_TRUE) {
        fputs("invalid benchmark arguments or adapter; use --help\n", stderr);
        return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    }
    result = dataset_create(&dataset, &options, adapter);
    if (result == BENCHMARK_CORE_STATUS_SUCCESS) {
        result = warm_up(&dataset, options.workload.warmup, &options, adapter, 0U);
    }
    if (result != BENCHMARK_CORE_STATUS_SUCCESS) {
        dataset_destroy(&dataset);
        return result;
    }
    result = options.measure_mode == MEASURE_KERNEL_ONLY
        ? execute_st_kernel_only(&dataset, options.iterations, &options, adapter, &elapsed, &checksum)
        : execute_st_end_to_end(&dataset, options.iterations, &options, adapter, &elapsed, &checksum);
    if (result != BENCHMARK_CORE_STATUS_SUCCESS) {
        dataset_destroy(&dataset);
        return result;
    }
    emit_st(adapter, &options, &dataset, options.iterations, checksum, elapsed);
    dataset_destroy(&dataset);
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Выполняет end-to-end portion одного MT worker.
 * @details Алгоритм выделяет private state, выбирает rows со worker offset, включает
 * source memcpy в operation lifecycle, накапливает private checksum/success и не
 * обращается к другим worker results до coordinator join.
 */
static benchmark_core_status_t execute_mt_end_to_end(benchmark_worker_t *worker, uint64_t iterations)
{
    unsigned char *state = malloc(worker->dataset->state_size);
    if (state == NULL) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    for (uint64_t iteration = 0U; iteration < iterations; ++iteration) {
        const size_t row = (size_t)((iteration + worker->worker_id) % worker->dataset->count);
        memcpy(state, dataset_item(worker->dataset, row), worker->dataset->state_size);
        if (invoke(worker->adapter, state, iteration, &worker->options->workload) != BENCHMARK_CORE_STATUS_SUCCESS) {
            free(state);
            return BENCHMARK_CORE_STATUS_CALLBACK_ERROR;
        }
        worker->checksum ^= worker->adapter->checksum(state, iteration, worker->adapter->adapter_context);
        ++worker->successful;
    }
    free(state);
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/** @brief Выполняет kernel-only portion одного MT worker.
 * @details Алгоритм batch-копирует source до начала каждого local timed interval,
 * измеряет только operation/checksum loop и суммирует private kernel duration.
 * Coordinator использует maximum worker duration как MT kernel elapsed.
 */
static benchmark_core_status_t execute_mt_kernel_only(benchmark_worker_t *worker, uint64_t iterations)
{
    unsigned char *workspace;
    uint64_t completed = 0U;
    size_t allocation_size;

    if (worker->dataset->count > SIZE_MAX / worker->dataset->state_size) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    allocation_size = worker->dataset->count * worker->dataset->state_size;
    workspace = malloc(allocation_size);
    if (workspace == NULL) return BENCHMARK_CORE_STATUS_ALLOCATION_ERROR;
    while (completed < iterations) {
        const size_t batch = (size_t)((iterations - completed) < worker->dataset->count
            ? (iterations - completed) : worker->dataset->count);
        struct timespec start;
        struct timespec end;

        memcpy(workspace, worker->dataset->sources, batch * worker->dataset->state_size);
        if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) { free(workspace); return BENCHMARK_CORE_STATUS_CLOCK_ERROR; }
        for (size_t row = 0U; row < batch; ++row) {
            void *state = workspace + row * worker->dataset->state_size;
            if (invoke(worker->adapter, state, completed + row, &worker->options->workload) != BENCHMARK_CORE_STATUS_SUCCESS) {
                free(workspace);
                return BENCHMARK_CORE_STATUS_CALLBACK_ERROR;
            }
            worker->checksum ^= worker->adapter->checksum(
                state, completed + row, worker->adapter->adapter_context);
            ++worker->successful;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) { free(workspace); return BENCHMARK_CORE_STATUS_CLOCK_ERROR; }
        worker->kernel_elapsed_seconds += seconds_between(&start, &end);
        completed += batch;
    }
    free(workspace);
    return BENCHMARK_CORE_STATUS_SUCCESS;
}

/**
 * @brief Исполняет warm-up, barrier wait и measurement lifecycle одного pthread.
 * @param opaque Pointer на benchmark_worker_t, созданный coordinator.
 * @return NULL после записи private result/failure в worker context.
 * @details
 * Алгоритм запускает warm-up, увеличивает ready counter под mutex и broadcast-ит
 * coordinator/peers. Затем condition-wait защищает от spurious wakeup до released
 * или aborted. После release worker выполняет selected boundary без shared mutable
 * state; coordinator читает fields только после pthread_join.
 */
static void *worker_main(void *opaque)
{
    benchmark_worker_t *worker = opaque;
    const uint64_t iterations = worker->options->total_iterations /
        (uint64_t)worker->options->threads;

    if (warm_up(worker->dataset, worker->options->workload.warmup, worker->options,
            worker->adapter, worker->worker_id) != BENCHMARK_CORE_STATUS_SUCCESS) {
        worker->failed = BENCHMARK_BOOLEAN_TRUE;
    }
    /* Publish warm-up completion before waiting; broadcast wakes coordinator and peers safely. */
    (void)pthread_mutex_lock(worker->mutex);
    ++*worker->ready_workers;
    (void)pthread_cond_broadcast(worker->condition);
    while (*worker->released != BENCHMARK_BOOLEAN_TRUE &&
        *worker->aborted != BENCHMARK_BOOLEAN_TRUE) {
        (void)pthread_cond_wait(worker->condition, worker->mutex);
    }
    {
        const benchmark_boolean_t should_abort = *worker->aborted;
        (void)pthread_mutex_unlock(worker->mutex);
        if (should_abort == BENCHMARK_BOOLEAN_TRUE) return NULL;
    }

    worker->checksum = 0U;
    worker->successful = 0U;
    worker->kernel_elapsed_seconds = 0.0;
    if (worker->failed != BENCHMARK_BOOLEAN_TRUE && (worker->options->measure_mode == MEASURE_KERNEL_ONLY
        ? execute_mt_kernel_only(worker, iterations)
        : execute_mt_end_to_end(worker, iterations)) != BENCHMARK_CORE_STATUS_SUCCESS) {
        worker->failed = BENCHMARK_BOOLEAN_TRUE;
    }
    return NULL;
}

/**
 * @brief Исполняет complete multi-thread benchmark lifecycle.
 * @details Алгоритм parses/validates options и adapter, создаёт immutable dataset,
 * allocates workers, запускает pthreads, ждёт readiness barrier, фиксирует common
 * start и releases workers. После joins aggregate checksum/success и выбирает max
 * worker kernel elapsed либо wall duration. Cleanup aborts и joins every created
 * worker exactly once, затем уничтожает synchronization primitives и dataset.
 */
benchmark_core_status_t benchmark_core_run_mt(int argc, char **argv, const benchmark_adapter_t *adapter)
{
    benchmark_options_t options;
    benchmark_dataset_t dataset;
    pthread_t *threads = NULL;
    benchmark_worker_t *workers = NULL;
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    struct timespec start;
    struct timespec end;
    size_t ready_workers = 0U;
    size_t created_workers = 0U;
    size_t joined_workers = 0U;
    benchmark_boolean_t released = BENCHMARK_BOOLEAN_FALSE;
    benchmark_boolean_t aborted = BENCHMARK_BOOLEAN_FALSE;
    uint64_t checksum = 0U;
    uint64_t successful = 0U;
    uint64_t iterations_per_thread;
    double kernel_elapsed = 0.0;
    benchmark_core_status_t status = BENCHMARK_CORE_STATUS_THREAD_ERROR;
    const benchmark_core_status_t parse_result = parse_options(
        argc, argv, BENCHMARK_BOOLEAN_TRUE, &options);

    if (parse_result == BENCHMARK_CORE_STATUS_HELP) return BENCHMARK_CORE_STATUS_SUCCESS;
    if (parse_result != BENCHMARK_CORE_STATUS_SUCCESS ||
        valid_adapter(adapter) != BENCHMARK_BOOLEAN_TRUE) {
        fputs("invalid benchmark arguments or adapter; use --help\n", stderr);
        return BENCHMARK_CORE_STATUS_ARGUMENT_ERROR;
    }
    status = dataset_create(&dataset, &options, adapter);
    if (status != BENCHMARK_CORE_STATUS_SUCCESS) return status;
    threads = calloc(options.threads, sizeof(*threads));
    workers = calloc(options.threads, sizeof(*workers));
    if (threads == NULL || workers == NULL) goto cleanup;
    for (size_t index = 0U; index < options.threads; ++index) {
        workers[index] = (benchmark_worker_t){
            .dataset = &dataset,
            .options = &options,
            .adapter = adapter,
            .worker_id = index,
            .mutex = &mutex,
            .condition = &condition,
            .ready_workers = &ready_workers,
            .released = &released,
            .aborted = &aborted
        };
        if (pthread_create(&threads[index], NULL, worker_main, &workers[index]) != 0) {
            (void)pthread_mutex_lock(&mutex);
            aborted = BENCHMARK_BOOLEAN_TRUE;
            (void)pthread_cond_broadcast(&condition);
            (void)pthread_mutex_unlock(&mutex);
            goto cleanup;
        }
        ++created_workers;
    }
    (void)pthread_mutex_lock(&mutex);
    /* Predicate loop handles spurious wakeups and starts timing only after every worker is ready. */
    while (ready_workers != options.threads) {
        (void)pthread_cond_wait(&condition, &mutex);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        aborted = 1;
        (void)pthread_cond_broadcast(&condition);
        (void)pthread_mutex_unlock(&mutex);
        goto cleanup;
    }
    released = BENCHMARK_BOOLEAN_TRUE;
    (void)pthread_cond_broadcast(&condition);
    (void)pthread_mutex_unlock(&mutex);
    for (size_t index = 0U; index < created_workers; ++index) {
        const int join_result = pthread_join(threads[index], NULL);
        ++joined_workers;
        if (join_result != 0 || workers[index].failed == BENCHMARK_BOOLEAN_TRUE) {
            status = BENCHMARK_CORE_STATUS_THREAD_ERROR;
            goto cleanup;
        }
        checksum ^= workers[index].checksum;
        successful += workers[index].successful;
        /* MT kernel throughput is bounded by the slowest worker, not an average partial duration. */
        if (workers[index].kernel_elapsed_seconds > kernel_elapsed) {
            kernel_elapsed = workers[index].kernel_elapsed_seconds;
        }
    }
    if (clock_gettime(CLOCK_MONOTONIC, &end) != 0) {
        status = BENCHMARK_CORE_STATUS_CLOCK_ERROR;
        goto cleanup;
    }
    iterations_per_thread = options.total_iterations / (uint64_t)options.threads;
    emit_mt(adapter, &options, &dataset, iterations_per_thread, successful, checksum,
        options.measure_mode == MEASURE_KERNEL_ONLY ? kernel_elapsed : seconds_between(&start, &end));
    status = BENCHMARK_CORE_STATUS_SUCCESS;

cleanup:
    if (created_workers > joined_workers) {
        /* Abort before join guarantees waiters leave the barrier on every error path. */
        (void)pthread_mutex_lock(&mutex);
        aborted = 1;
        (void)pthread_cond_broadcast(&condition);
        (void)pthread_mutex_unlock(&mutex);
        for (size_t index = joined_workers; index < created_workers; ++index) {
            (void)pthread_join(threads[index], NULL);
        }
    }
    free(workers);
    free(threads);
    dataset_destroy(&dataset);
    (void)pthread_cond_destroy(&condition);
    (void)pthread_mutex_destroy(&mutex);
    return status;
}
