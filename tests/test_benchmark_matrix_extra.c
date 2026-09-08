/**
 * @file test_benchmark_matrix_extra.c
 * @brief Дополнительные и fuzz-тесты для библиотеки benchmark_matrix.
 * @details Проверяет таймауты, краши дочерних процессов, ошибки I/O, 
 * превышение лимитов (MAX_PROFILES), дубликаты ID и парсинг мусорных данных.
 */

#define _POSIX_C_SOURCE 200809L

#include "../include/benchmark_matrix.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Вспомогательная функция для создания файлов */
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

static void delete_file(const char *path)
{
    unlink(path);
}

static void init_default_options(options_t *opts)
{
    memset(opts, 0, sizeof(options_t));
    opts->repetitions = 1;
    opts->iterations = 10;
    opts->mt_total_iterations = 10;
    opts->threads = 1;
    opts->warmup = 1;
    opts->data_count = 10;
    opts->seed = 42;
    opts->timeout_seconds = 1.0; /* Короткий таймаут для тестов */
}

/* --- Тесты --- */

static void test_duplicate_profiles(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_dup_manifest.json";
    /* Манифест с двумя одинаковыми ID "prof1" */
    const char *manifest_json = 
        "{\"schema_version\": 1, \"profiles\": ["
        "{\"id\": \"prof1\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e\"},"
        "{\"id\": \"prof1\", \"input_kind\": \"x\", \"operation_kind\": \"y\", \"measure_mode\": \"z\", \"size_profile\": \"w\", \"capacity_profile\": \"q\"}"
        "]}";
    write_file(manifest_path, manifest_json, 0);
    
    options_t opts;
    int failures;
    uint64_t samples;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = "/bin/true";
    opts.mt_binary = "/bin/true";
    opts.output = "test_out.json";
    
    /* Библиотека должна обнаружить дубликат и вернуть ошибку аргументов */
    assert(bench_matrix_execute(&opts, &failures, &samples) == BENCH_MATRIX_STATUS_ARGUMENT_ERROR);
    
    delete_file(manifest_path);
}

static void test_too_many_profiles(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_huge_manifest.json";
    
    /* Генерируем JSON с количеством профилей > MAX_PROFILES (128) */
    FILE *f = fopen(manifest_path, "w");
    assert(f != NULL);
    fputs("{\"schema_version\": 1, \"profiles\": [", f);
    for (int i = 0; i < 130; ++i) {
        fprintf(f, "{\"id\": \"p%d\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e\"}%s", 
                i, (i < 129) ? "," : "");
    }
    fputs("]}", f);
    fclose(f);
    
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

static void test_manifest_fuzz_corpus(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_fuzz_manifest.json";
    
    /* Набор некорректных JSON для проверки устойчивости парсера */
    const char *corpus[] = {
        "", /* Пустой файл */
        "{", /* Неожиданный конец */
        "{\"schema_version\": 1, \"profiles\": [{\"id\": \"p1\"}]}", /* Отсутствуют обязательные поля */
        "{\"schema_version\": \"1\", \"profiles\": []}", /* Неверный тип schema_version (строка вместо числа) */
        "{\"schema_version\": 1, \"profiles\": \"not_an_array\"}", /* profiles не массив */
        "{\"schema_version\": 1, \"profiles\": [{\"id\": \"p1\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e  with spaces\"}]}", /* Пробелы запрещены в значениях */
        NULL
    };
    
    options_t opts;
    int failures;
    uint64_t samples;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = "/bin/true";
    opts.mt_binary = "/bin/true";
    opts.output = "test_out.json";
    
    for (int i = 0; corpus[i] != NULL; ++i) {
        write_file(manifest_path, corpus[i], 0);
        assert(bench_matrix_execute(&opts, &failures, &samples) == BENCH_MATRIX_STATUS_ARGUMENT_ERROR);
    }
    
    delete_file(manifest_path);
}

static void test_child_timeout(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_valid_manifest.json";
    const char *sleep_bin = "./test_sleep.sh";
    const char *out_path = "test_out_timeout.json";
    
    write_file(manifest_path, "{\"schema_version\": 1, \"profiles\": [{\"id\": \"p1\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e\"}]}", 0);
    
    /* Скрипт спит дольше, чем timeout_seconds (1.0) */
    write_file(sleep_bin, "#!/bin/sh\nsleep 3\nexit 0\n", 1);
    
    options_t opts;
    int failures = 0;
    uint64_t samples = 0;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = sleep_bin;
    opts.mt_binary = sleep_bin;
    opts.output = out_path;
    opts.timeout_seconds = 0.5; /* Устанавливаем жесткий таймаут */
    
    bench_matrix_status_t status = bench_matrix_execute(&opts, &failures, &samples);
    
    /* Матрица должна завершиться успешно, но зафиксировать failures из-за таймаута (SIGKILL) */
    assert(status == BENCH_MATRIX_STATUS_SUCCESS);
    assert(failures == 2); /* 1 профиль * 2 режима */
    assert(samples == 2);
    
    delete_file(manifest_path);
    delete_file(sleep_bin);
    delete_file(out_path);
}

static void test_child_crash(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_valid_manifest.json";
    const char *crash_bin = "./test_crash.sh";
    const char *out_path = "test_out_crash.json";
    
    write_file(manifest_path, "{\"schema_version\": 1, \"profiles\": [{\"id\": \"p1\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e\"}]}", 0);
    
    /* Скрипт завершается с ошибкой */
    write_file(crash_bin, "#!/bin/sh\necho \"I am crashing\"\nexit 139\n", 1);
    
    options_t opts;
    int failures = 0;
    uint64_t samples = 0;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = crash_bin;
    opts.mt_binary = crash_bin;
    opts.output = out_path;
    
    bench_matrix_status_t status = bench_matrix_execute(&opts, &failures, &samples);
    
    assert(status == BENCH_MATRIX_STATUS_SUCCESS);
    assert(failures == 2);
    
    delete_file(manifest_path);
    delete_file(crash_bin);
    delete_file(out_path);
}

static void test_malformed_protocol_fuzz(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_valid_manifest.json";
    const char *bad_proto_bin = "./test_bad_proto.sh";
    const char *out_path = "test_out_bad_proto.json";
    
    write_file(manifest_path, "{\"schema_version\": 1, \"profiles\": [{\"id\": \"p1\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e\"}]}", 0);
    
    /* Скрипт выдает мусор вместо чисел */
    const char *script = 
        "#!/bin/sh\n"
        "echo \"benchmark=test elapsed_seconds=NOT_A_NUMBER ns_per_call=10.0\"\n"
        "echo \"Benchmark finished.\"\n"
        "exit 0\n";
    write_file(bad_proto_bin, script, 1);
    
    options_t opts;
    int failures = 0;
    uint64_t samples = 0;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = bad_proto_bin;
    opts.mt_binary = bad_proto_bin;
    opts.output = out_path;
    
    bench_matrix_status_t status = bench_matrix_execute(&opts, &failures, &samples);
    
    /* Парсер протокола должен отклонить NOT_A_NUMBER */
    assert(status == BENCH_MATRIX_STATUS_SUCCESS);
    assert(failures == 2);
    
    delete_file(manifest_path);
    delete_file(bad_proto_bin);
    delete_file(out_path);
}

static void test_output_io_error(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_valid_manifest.json";
    write_file(manifest_path, "{\"schema_version\": 1, \"profiles\": [{\"id\": \"p1\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e\"}]}", 0);
    
    options_t opts;
    int failures;
    uint64_t samples;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = "/bin/true";
    opts.mt_binary = "/bin/true";
    
    /* Пытаемся записать в директорию, что вызовет ошибку открытия файла (EISDIR или EACCES) */
    opts.output = "/"; 
    
    assert(bench_matrix_execute(&opts, &failures, &samples) == BENCH_MATRIX_STATUS_IO_ERROR);
    
    delete_file(manifest_path);
}

int main(void)
{
    printf("Starting benchmark_matrix extra/fuzz tests...\n");
    
    test_duplicate_profiles();
    test_too_many_profiles();
    test_manifest_fuzz_corpus();
    test_child_timeout();
    test_child_crash();
    test_malformed_protocol_fuzz();
    test_output_io_error();
    
    printf("All benchmark_matrix extra tests passed successfully!\n");
    return 0;
}
