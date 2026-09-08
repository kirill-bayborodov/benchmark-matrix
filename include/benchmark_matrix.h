/**
 * @file benchmark_matrix.h
 * @brief C11 executor declared benchmark matrix и raw JSON artifact writer.
 * @details Заголовочный файл библиотеки для запуска benchmark-матрицы.
 */

#ifndef BENCHMARK_MATRIX_H
#define BENCHMARK_MATRIX_H

#include <stddef.h>
#include <stdint.h>

#define MAX_PROFILES 128U
#define FIELD_SIZE 128U
#define CAPTURE_SIZE (1024U * 1024U)

/**
 * @brief Описывает результат внутренней операции matrix runner и его ISO C exit mapping.
 * @details Все fallible tool helpers возвращают этот enum.
 */
typedef enum {
    BENCH_MATRIX_STATUS_SUCCESS = 0,          /**< Операция успешно завершена. */
    BENCH_MATRIX_STATUS_HELP = 1,             /**< Запрошена справка; запуск matrix не требуется. */
    BENCH_MATRIX_STATUS_ARGUMENT_ERROR = 2,   /**< CLI, profile manifest или output argument некорректен. */
    BENCH_MATRIX_STATUS_ALLOCATION_ERROR = 3, /**< Bounded heap allocation не удалась. */
    BENCH_MATRIX_STATUS_PROCESS_ERROR = 4,    /**< fork, exec, protocol или child lifecycle завершился ошибкой. */
    BENCH_MATRIX_STATUS_IO_ERROR = 5          /**< JSON artifact I/O или system metadata операция не удалась. */
} bench_matrix_status_t;

/** 
 * @brief Представляет именованный boolean matrix/protocol predicate. 
 */
typedef enum {
    BENCH_MATRIX_BOOLEAN_FALSE = 0, /**< Предикат не выполнен. */
    BENCH_MATRIX_BOOLEAN_TRUE = 1   /**< Предикат выполнен. */
} bench_matrix_boolean_t;

/**
 * @brief Хранит один валидированный manifest workload profile.
 * @details Каждое поле копируется из JSON и проходит token validation до построения argv.
 */
typedef struct {
    char id[FIELD_SIZE];               /**< Unique stable profile identifier for artifact grouping. */
    char input_kind[FIELD_SIZE];       /**< Adapter input pattern forwarded as `--input-kind`. */
    char operation_kind[FIELD_SIZE];   /**< Adapter operation selector forwarded as `--operation-kind`. */
    char measure_mode[FIELD_SIZE];     /**< Measurement boundary forwarded as `--measure-mode`. */
    char size_profile[FIELD_SIZE];     /**< Operand length scenario forwarded as `--size-profile`. */
    char capacity_profile[FIELD_SIZE]; /**< Storage-capacity scenario forwarded as `--capacity-profile`. */
} profile_t;

/**
 * @brief Хранит конфигурацию matrix execution.
 * @details Структура объединяет paths, repetitions и воспроизводимые workload параметры.
 */
typedef struct {
    const char *manifest;         /**< Input versioned JSON workload profile manifest. */
    const char *output;           /**< Destination path for atomically published raw JSON artifact. */
    const char *st_binary;        /**< Executable implementing the ST benchmark-core protocol. */
    const char *mt_binary;        /**< Executable implementing the MT benchmark-core protocol. */
    uint64_t repetitions;         /**< Independent executions for every profile and mode. */
    uint64_t iterations;          /**< Per-run ST iterations forwarded to the child. */
    uint64_t mt_total_iterations; /**< Total MT iterations, exactly divisible by threads. */
    uint64_t warmup;              /**< Untimed warm-up operations forwarded to every benchmark. */
    uint64_t data_count;          /**< Pregenerated dataset cardinality forwarded to every benchmark. */
    uint64_t seed;                /**< Deterministic data-generation seed forwarded to every benchmark. */
    size_t threads;               /**< MT worker count forwarded to the MT binary. */
    double timeout_seconds;       /**< Parent-side wall-clock timeout for one child process. */
} options_t;

/**
 * @brief Хранит результат одного дочернего benchmark process.
 */
typedef struct {
    int returncode;                     /**< Raw waited child exit code or signal-derived failure indication. */
    bench_matrix_boolean_t protocol_ok; /**< Whether required benchmark record and completion marker were captured. */
    char error[160];                    /**< Bounded diagnostic explaining a process or protocol failure. */
    char benchmark[FIELD_SIZE];         /**< Parsed benchmark name from the machine-readable record. */
    double elapsed_seconds;             /**< Parsed elapsed wall time in seconds. */
    double ns_per_call;                 /**< Parsed normalized duration in nanoseconds per operation. */
    char *output;                       /**< Owned bounded stdout/stderr capture; caller releases it with free. */
} result_t;

/**
 * @brief Выполняет полный declared benchmark matrix на основе заданных опций.
 * @param options Указатель на структуру с валидированными настройками.
 * @param failures Указатель для возврата количества ошибок протокола.
 * @param sample_count Указатель для возврата количества успешно собранных сэмплов.
 * @return BENCH_MATRIX_STATUS_SUCCESS при успехе, иначе код ошибки.
 */
bench_matrix_status_t bench_matrix_execute(const options_t *options, int *failures, uint64_t *sample_count);

#endif /* BENCHMARK_MATRIX_H */
