/**
 * @file test_benchmark_matrix_runner.c
 * @brief Интеграционные тесты для CLI-интерфейса benchmark_matrix.
 * @details Запускает собранный исполняемый файл ./bench_matrix через system()
 * и проверяет коды возврата (0 - успех/help, 1 - ошибки протокола, 2 - ошибки аргументов/IO).
 */

#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

/* Вспомогательная функция для запуска CLI и получения кода возврата.
 * Вывод stdout и stderr подавляется, чтобы не засорять логи тестов. */
static int run_cli(const char *args)
{
    char command[1024];
    /* Предполагается, что тесты запускаются из /bin проекта (make test) */
    snprintf(command, sizeof(command), "bin/bench_matrix %s > /dev/null 2>&1", args);
    
    int status = system(command);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1; /* Процесс упал или был убит сигналом */
}


/* --- Тесты --- */

static void test_help_flag(void)
{
    printf("Running %s...\n", __func__);
    /* Флаг --help должен возвращать 0 */
    assert(run_cli("--help") == 0);
}

static void test_no_arguments(void)
{
    printf("Running %s...\n", __func__);
    /* Без аргументов утилита должна вернуть 2 (ARGUMENT_ERROR) */
    assert(run_cli("") == 2);
}

static void test_unknown_flag(void)
{
    printf("Running %s...\n", __func__);
    /* Неизвестный флаг должен вернуть 2 */
    assert(run_cli("--unknown-flag 123") == 2);
}

static void test_missing_mandatory_args(void)
{
    printf("Running %s...\n", __func__);
    /* Указаны не все обязательные аргументы */
    assert(run_cli("--manifest manifest.json --output out.json") == 2);
}

static void test_invalid_number_format(void)
{
    printf("Running %s...\n", __func__);
    /* Передаем строку вместо числа в --threads */
    const char *args = "--manifest m.json --output o.json --st-binary a --mt-binary b --threads abc";
    assert(run_cli(args) == 2);
    
    /* Передаем 0 в --threads (должно быть строго положительным) */
    const char *args_zero = "--manifest m.json --output o.json --st-binary a --mt-binary b --threads 0";
    assert(run_cli(args_zero) == 2);
}

static void test_mt_iterations_divisibility(void)
{
    printf("Running %s...\n", __func__);
    /* mt_total_iterations (10) не кратно threads (3). 
     * parse_options должен отловить это до запуска процессов. */
    const char *args = "--manifest m.json --output o.json --st-binary a --mt-binary b "
                       "--threads 3 --mt-total-iterations 10";
    assert(run_cli(args) == 2);
}

static void test_missing_manifest_file(void)
{
    printf("Running %s...\n", __func__);
    /* Аргументы валидны, но файлы не существуют. 
     * Ошибка должна быть отловлена на этапе load_profiles или access() -> код 2. */
    const char *args = "--manifest does_not_exist.json --output out.json "
                       "--st-binary /bin/true --mt-binary /bin/true";
    assert(run_cli(args) == 2);
}

static void test_all_optional_arguments(void)
{
    printf("Running %s...\n", __func__);
    
    /* Используем файлы, сгенерированные Makefile */
    const char *manifest_path = "build/fixtures/mock_manifest.json";
    const char *bin_path = "build/fixtures/mock_bin.sh";
    const char *out_path = "build/fixtures/out.json"; /* Сюда утилита запишет результат */

    char args[2048];
    snprintf(args, sizeof(args), 
             "--manifest %s --output %s --st-binary %s --mt-binary %s "
             "--repetitions 2 --iterations 500 --mt-total-iterations 1000 "
             "--threads 2 --warmup 50 --data-count 2048 --seed 9999 --timeout-seconds 2.5",
             manifest_path, out_path, bin_path, bin_path);
                       
    /* Запускаем CLI. Ожидаем код 0 */
    int exit_code = run_cli(args);

    /* Удаляем только выходной файл, чтобы не мешать следующим запускам.
       Манифест и бинарник оставляем — они управляются Makefile. */
    remove(out_path);

    assert(exit_code == 0);
}

int main(void)
{
    printf("Starting CLI runner tests...\n");
    
    /* Проверяем, существует ли бинарник перед запуском тестов */
    if (system("test -x bin/bench_matrix") != 0) {
        fprintf(stderr, "Error: bin/bench_matrix executable not found. Run 'make' first.\n");
        return 1;
    }

    test_help_flag();
    test_no_arguments();
    test_unknown_flag();
    test_missing_mandatory_args();
    test_invalid_number_format();
    test_mt_iterations_divisibility();
    test_missing_manifest_file();
    test_all_optional_arguments();
    
    printf("All CLI runner tests passed successfully!\n");
    return 0;
}
