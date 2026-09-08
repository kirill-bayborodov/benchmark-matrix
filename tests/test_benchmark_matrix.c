/**
 * @file test_benchmark_matrix.c
 * @brief Юнит-тесты для библиотеки benchmark_matrix.
 * @details Тестирует валидацию аргументов, парсинг манифестов, успешное выполнение
 * процессов и обработку ошибок протокола/процессов с помощью mock-скриптов.
 */

#define _POSIX_C_SOURCE 200809L

#include "../include/benchmark_matrix.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Вспомогательная функция для создания текстовых файлов (манифестов и скриптов) */
static void write_file(const char *path, const char *content, int executable)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL && "Failed to create test file");
    fputs(content, f);
    fclose(f);
    if (executable) {
        chmod(path, 0755);
    }
}

/* Вспомогательная функция для удаления файлов */
static void delete_file(const char *path)
{
    unlink(path);
}

/* Базовые настройки для тестов */
static void init_default_options(options_t *opts)
{
    memset(opts, 0, sizeof(options_t));
    opts->repetitions = 1;
    opts->iterations = 100;
    opts->mt_total_iterations = 100;
    opts->threads = 1;
    opts->warmup = 10;
    opts->data_count = 100;
    opts->seed = 42;
    opts->timeout_seconds = 2.0;
}

/* --- Тестовые сценарии --- */

static void test_null_arguments(void)
{
    printf("Running %s...\n", __func__);
    options_t opts;
    int failures;
    uint64_t samples;
    
    assert(bench_matrix_execute(NULL, &failures, &samples) == BENCH_MATRIX_STATUS_ARGUMENT_ERROR);
    assert(bench_matrix_execute(&opts, NULL, &samples) == BENCH_MATRIX_STATUS_ARGUMENT_ERROR);
    assert(bench_matrix_execute(&opts, &failures, NULL) == BENCH_MATRIX_STATUS_ARGUMENT_ERROR);
}

static void test_missing_manifest_or_binaries(void)
{
    printf("Running %s...\n", __func__);
    options_t opts;
    int failures;
    uint64_t samples;
    init_default_options(&opts);
    
    opts.manifest = "non_existent_manifest.json";
    opts.st_binary = "non_existent_bin";
    opts.mt_binary = "non_existent_bin";
    opts.output = "test_out.json";
    
    assert(bench_matrix_execute(&opts, &failures, &samples) == BENCH_MATRIX_STATUS_ARGUMENT_ERROR);
}

static void test_invalid_manifest_schema(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_invalid_manifest.json";
    write_file(manifest_path, "{\"schema_version\": 999, \"profiles\": []}", 0);
    
    options_t opts;
    int failures;
    uint64_t samples;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = "/bin/true";
    opts.mt_binary = "/bin/true";
    opts.output = "test_out.json";
    
    assert(bench_matrix_execute(&opts, &failures, &samples) == BENCH_MATRIX_STATUS_ARGUMENT_ERROR);
    
    delete_file(manifest_path);
}

static void test_successful_execution(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_valid_manifest.json";
    const char *dummy_bin = "./test_dummy_ok.sh";
    const char *out_path = "test_out_success.json";
    
    /* Валидный манифест с одним профилем */
    const char *manifest_json = 
        "{\"schema_version\": 1, \"profiles\": [{"
        "\"id\": \"prof1\", \"input_kind\": \"seq\", \"operation_kind\": \"read\","
        "\"measure_mode\": \"all\", \"size_profile\": \"small\", \"capacity_profile\": \"huge\""
        "}]}";
    write_file(manifest_path, manifest_json, 0);
    
    /* Mock-скрипт, который выводит правильный протокол */
    const char *script = 
        "#!/bin/sh\n"
        "echo \"benchmark=dummy_bench elapsed_seconds=0.001 ns_per_call=15.5\"\n"
        "echo \"Benchmark finished.\"\n"
        "exit 0\n";
    write_file(dummy_bin, script, 1);
    
    options_t opts;
    int failures = -1;
    uint64_t samples = 0;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = dummy_bin;
    opts.mt_binary = dummy_bin;
    opts.output = out_path;
    
    bench_matrix_status_t status = bench_matrix_execute(&opts, &failures, &samples);
    
    assert(status == BENCH_MATRIX_STATUS_SUCCESS);
    assert(failures == 0);
    assert(samples == 2); /* 1 профиль * 2 режима (st, mt) * 1 повторение */
    
    delete_file(manifest_path);
    delete_file(dummy_bin);
    delete_file(out_path);
}

static void test_protocol_failure(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_valid_manifest.json";
    const char *dummy_bin = "./test_dummy_fail.sh";
    const char *out_path = "test_out_fail.json";
    
    const char *manifest_json = 
        "{\"schema_version\": 1, \"profiles\": [{"
        "\"id\": \"prof1\", \"input_kind\": \"seq\", \"operation_kind\": \"read\","
        "\"measure_mode\": \"all\", \"size_profile\": \"small\", \"capacity_profile\": \"huge\""
        "}]}";
    write_file(manifest_path, manifest_json, 0);
    
    /* Mock-скрипт, который завершается успешно, но не выводит нужный протокол */
    const char *script = 
        "#!/bin/sh\n"
        "echo \"Some garbage output\"\n"
        "exit 0\n";
    write_file(dummy_bin, script, 1);
    
    options_t opts;
    int failures = 0;
    uint64_t samples = 0;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = dummy_bin;
    opts.mt_binary = dummy_bin;
    opts.output = out_path;
    
    bench_matrix_status_t status = bench_matrix_execute(&opts, &failures, &samples);
    
    /* Функция должна вернуть SUCCESS (матрица отработала), но зафиксировать ошибки протокола */
    assert(status == BENCH_MATRIX_STATUS_SUCCESS);
    assert(failures == 2); /* Ошибка в ST и MT запусках */
    assert(samples == 2);
    
    delete_file(manifest_path);
    delete_file(dummy_bin);
    delete_file(out_path);
}

int main(void)
{
    printf("Starting benchmark_matrix tests...\n");
    
    test_null_arguments();
    test_missing_manifest_or_binaries();
    test_invalid_manifest_schema();
    test_successful_execution();
    test_protocol_failure();
    
    printf("All benchmark_matrix tests passed successfully!\n");
    return 0;
}
