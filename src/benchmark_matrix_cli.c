/**
 * @file benchmark_matrix_cli.c
 * @brief Код реализации CLI-утилиты для benchmark matrix.
 * @details Содержит точку входа (main), логику разбора аргументов командной строки
 * и вывод пользовательской справки.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "benchmark_matrix_cli.h"
#include "benchmark_matrix.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void usage(const char *name)
{
    fprintf(stderr, "usage: %s --manifest FILE --output FILE --st-binary FILE --mt-binary FILE [--repetitions N] [--iterations N] [--mt-total-iterations N] [--threads N] [--warmup N] [--data-count N] [--seed N] [--timeout-seconds N]\n", name);
}

/** 
 * @brief Разбирает unsigned integer CLI value.
 * @details Алгоритм использует strtoull base 0, проверяет errno и полное
 * consumption, сохраняя support decimal и hexadecimal reproducibility seeds.
 */
static bench_matrix_status_t number_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    if (text == NULL || value == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    *value = (uint64_t)parsed;
    return BENCH_MATRIX_STATUS_SUCCESS;
}

/** 
 * @brief Разбирает строго положительный size_t CLI value.
 * @details Алгоритм делегирует uint64 parsing, затем проверяет zero и overflow
 * относительно SIZE_MAX до преобразования к platform-sized type.
 */
static bench_matrix_status_t number_size(const char *text, size_t *value)
{
    uint64_t parsed;
    if (value == NULL || number_u64(text, &parsed) != BENCH_MATRIX_STATUS_SUCCESS ||
        parsed == 0U || parsed > SIZE_MAX) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    *value = (size_t)parsed;
    return BENCH_MATRIX_STATUS_SUCCESS;
}

/** 
 * @brief Разбирает положительный finite timeout CLI value.
 * @details Алгоритм использует strtod и принимает только полностью consumed
 * positive text, исключая infinite, zero и malformed timeout configuration.
 */
static bench_matrix_status_t number_double(const char *text, double *value)
{
    char *end = NULL;
    if (text == NULL || value == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    errno = 0;
    *value = strtod(text, &end);
    return errno == 0 && end != text && *end == '\0' && *value > 0.0
        ? BENCH_MATRIX_STATUS_SUCCESS : BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
}

bench_matrix_status_t parse_options(int argc, char **argv, options_t *options)
{
    *options = (options_t){ .repetitions = 7U, .iterations = 200000000U,
        .mt_total_iterations = 320000000U, .threads = 2U, .warmup = 10000U,
        .data_count = 4096U, .seed = UINT64_C(0x9E3779B97F4A7C15), .timeout_seconds = 1800.0 };
    for (int index = 1; index < argc; ++index) {
        const char *key = argv[index];
        const char *value;
        if (strcmp(key, "--help") == 0) return BENCH_MATRIX_STATUS_HELP;
        if (++index >= argc) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
        value = argv[index];
        if (strcmp(key, "--manifest") == 0) options->manifest = value;
        else if (strcmp(key, "--output") == 0) options->output = value;
        else if (strcmp(key, "--st-binary") == 0) options->st_binary = value;
        else if (strcmp(key, "--mt-binary") == 0) options->mt_binary = value;
        else if (strcmp(key, "--repetitions") == 0) { if (number_u64(value, &options->repetitions) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else if (strcmp(key, "--iterations") == 0) { if (number_u64(value, &options->iterations) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else if (strcmp(key, "--mt-total-iterations") == 0) { if (number_u64(value, &options->mt_total_iterations) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else if (strcmp(key, "--threads") == 0) { if (number_size(value, &options->threads) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else if (strcmp(key, "--warmup") == 0) { if (number_u64(value, &options->warmup) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else if (strcmp(key, "--data-count") == 0) { if (number_u64(value, &options->data_count) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else if (strcmp(key, "--seed") == 0) { if (number_u64(value, &options->seed) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else if (strcmp(key, "--timeout-seconds") == 0) { if (number_double(value, &options->timeout_seconds) != BENCH_MATRIX_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR; }
        else return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    }
    return options->manifest != NULL && options->output != NULL && options->st_binary != NULL && options->mt_binary != NULL &&
        options->repetitions > 0U && options->iterations > 0U && options->mt_total_iterations > 0U &&
        options->data_count > 0U && options->threads > 0U && options->mt_total_iterations % options->threads == 0U
        ? BENCH_MATRIX_STATUS_SUCCESS : BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
}

/**
 * @brief Выполняет полный declared benchmark matrix.
 * @param argc Число CLI arguments.
 * @param argv CLI argument vector.
 * @return 0 при успешных samples; 1 при process/protocol failure; 2 при config/I/O error.
 * @details Алгоритм валидирует CLI, вызывает библиотечную функцию исполнения матрицы
 * и обрабатывает итоговые результаты и ошибки.
 */
int main(int argc, char **argv)
{
    options_t options;
    uint64_t sample_count = 0U;
    int failures = 0;
    bench_matrix_status_t status;

    status = parse_options(argc, argv, &options);
    if (status == BENCH_MATRIX_STATUS_HELP) { 
        usage(argv[0]); 
        return 0; 
    }
    if (status != BENCH_MATRIX_STATUS_SUCCESS) {
        usage(argv[0]);
        return 2;
    }

    status = bench_matrix_execute(&options, &failures, &sample_count);
    
    if (status == BENCH_MATRIX_STATUS_ARGUMENT_ERROR) {
        usage(argv[0]);
        return 2;
    } else if (status != BENCH_MATRIX_STATUS_SUCCESS) {
        /* I/O or internal error occurred (messages are printed by the library) */
        return 2;
    }

    if (failures != 0) { 
        fprintf(stderr, "bench_matrix: failures=%d; see %s\n", failures, options.output); 
        return 1; 
    }
    
    printf("bench_matrix: wrote %" PRIu64 " samples to %s\n", sample_count, options.output);
    return 0;
}
