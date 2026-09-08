/**
 * @file test_benchmark_matrix_mt.c
 * @brief Тесты многопоточного (MT) режима для библиотеки benchmark_matrix.
 * @details Проверяет передачу аргументов и способность библиотеки корректно
 * захватывать вывод и дожидаться завершения реальных многопоточных дочерних процессов.
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
    opts->iterations = 1000;
    opts->mt_total_iterations = 8000; /* 8 потоков * 1000 итераций */
    opts->threads = 8;
    opts->warmup = 100;
    opts->data_count = 1024;
    opts->seed = 12345;
    opts->timeout_seconds = 5.0;
}

/* --- Тест 1: Проверка правильности передачи аргументов --- */
static void test_mt_arguments_passing(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_mt_manifest.json";
    const char *st_bin = "./test_st_mock.sh";
    const char *mt_bin = "./test_mt_mock.sh";
    const char *out_path = "test_mt_out.json";
    
    const char *manifest_json = 
        "{\"schema_version\": 1, \"profiles\": [{"
        "\"id\": \"mt_prof\", \"input_kind\": \"seq\", \"operation_kind\": \"write\","
        "\"measure_mode\": \"all\", \"size_profile\": \"large\", \"capacity_profile\": \"huge\""
        "}]}";
    write_file(manifest_path, manifest_json, 0);
    
    const char *st_script = 
        "#!/bin/sh\n"
        "echo \"benchmark=st_bench elapsed_seconds=0.1 ns_per_call=10.0\"\n"
        "echo \"Benchmark finished.\"\n"
        "exit 0\n";
    write_file(st_bin, st_script, 1);

    /* Mock для MT-бинарника: строго проверяет наличие --threads 8 и --total-iterations 8000 */
    const char *mt_script = 
        "#!/bin/sh\n"
        "if [ \"$1\" != \"--threads\" ] || [ \"$2\" != \"8\" ]; then\n"
        "  echo \"MT Mock failed: expected --threads 8, got $1 $2\" >&2\n"
        "  exit 1\n"
        "fi\n"
        "if [ \"$3\" != \"--total-iterations\" ] || [ \"$4\" != \"8000\" ]; then\n"
        "  echo \"MT Mock failed: expected --total-iterations 8000, got $3 $4\" >&2\n"
        "  exit 1\n"
        "fi\n"
        "echo \"benchmark=mt_bench elapsed_seconds=0.05 ns_per_call=5.0\"\n"
        "echo \"Benchmark finished.\"\n"
        "exit 0\n";
    write_file(mt_bin, mt_script, 1);
    
    options_t opts;
    int failures = -1;
    uint64_t samples = 0;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = st_bin;
    opts.mt_binary = mt_bin;
    opts.output = out_path;
    
    bench_matrix_status_t status = bench_matrix_execute(&opts, &failures, &samples);
    
    assert(status == BENCH_MATRIX_STATUS_SUCCESS);
    assert(failures == 0);
    assert(samples == 2);
    
    delete_file(manifest_path);
    delete_file(st_bin);
    delete_file(mt_bin);
    delete_file(out_path);
}

/* --- Тест 2: Работа с реальным многопоточным дочерним процессом --- */
static void test_mt_real_concurrent_child(void)
{
    printf("Running %s...\n", __func__);
    const char *manifest_path = "test_mt_real_manifest.json";
    const char *worker_src = "test_mt_worker.c";
    const char *worker_bin = "./test_mt_worker";
    const char *out_path = "test_mt_real_out.json";
    
    write_file(manifest_path, "{\"schema_version\": 1, \"profiles\": [{\"id\": \"p1\", \"input_kind\": \"a\", \"operation_kind\": \"b\", \"measure_mode\": \"c\", \"size_profile\": \"d\", \"capacity_profile\": \"e\"}]}", 0);
    
    /* Исходный код реального многопоточного приложения на C */
    const char *c_code = 
        "#include <pthread.h>\n"
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <unistd.h>\n"
        "void* worker(void* arg) {\n"
        "    usleep(50000); /* Эмуляция работы (50мс) */\n"
        "    return NULL;\n"
        "}\n"
        "int main(int argc, char** argv) {\n"
        "    int threads = 1;\n"
        "    for (int i = 1; i < argc; i++) {\n"
        "        if (strcmp(argv[i], \"--threads\") == 0 && i + 1 < argc) threads = atoi(argv[i+1]);\n"
        "    }\n"
        "    pthread_t* tids = malloc(threads * sizeof(pthread_t));\n"
        "    for (int i = 0; i < threads; i++) pthread_create(&tids[i], NULL, worker, NULL);\n"
        "    for (int i = 0; i < threads; i++) pthread_join(tids[i], NULL);\n"
        "    free(tids);\n"
        "    printf(\"benchmark=real_mt_bench elapsed_seconds=0.05 ns_per_call=1.2\\n\");\n"
        "    printf(\"Benchmark finished.\\n\");\n"
        "    return 0;\n"
        "}\n";
    write_file(worker_src, c_code, 0);
    
    /* Компилируем многопоточный бинарник */
    int compile_res = system("gcc -pthread test_mt_worker.c -o test_mt_worker");
    assert(compile_res == 0 && "Failed to compile MT worker");
    
    options_t opts;
    int failures = -1;
    uint64_t samples = 0;
    init_default_options(&opts);
    opts.manifest = manifest_path;
    opts.st_binary = worker_bin; /* Используем его и для ST, и для MT */
    opts.mt_binary = worker_bin;
    opts.output = out_path;
    opts.threads = 16; /* Запускаем 16 потоков */
    
    bench_matrix_status_t status = bench_matrix_execute(&opts, &failures, &samples);
    
    /* Библиотека должна успешно дождаться завершения всех 16 потоков, 
       не зависнуть на чтении пайпа и корректно распарсить вывод. */
    assert(status == BENCH_MATRIX_STATUS_SUCCESS);
    assert(failures == 0);
    assert(samples == 2);
    
    delete_file(manifest_path);
    delete_file(worker_src);
    delete_file(worker_bin);
    delete_file(out_path);
}

int main(void)
{
    printf("Starting benchmark_matrix MT tests...\n");
    
    test_mt_arguments_passing();
    test_mt_real_concurrent_child();
    
    printf("All benchmark_matrix MT tests passed successfully!\n");
    return 0;
}
