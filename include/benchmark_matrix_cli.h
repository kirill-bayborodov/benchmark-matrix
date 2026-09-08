/**
 * @file benchmark_matrix_cli.h
 * @brief Заголовочный файл для CLI-утилиты benchmark matrix.
 * @details Содержит объявления функций для разбора аргументов командной строки
 * и вывода справочной информации.
 */

#ifndef BENCHMARK_MATRIX_CLI_H
#define BENCHMARK_MATRIX_CLI_H

#include "benchmark_matrix.h"

/** 
 * @brief Печатает детерминированную CLI usage строку.
 * @param name Имя исполняемого файла (обычно argv[0]).
 * @details Алгоритм не изменяет state и направляет синтаксис в stderr, поэтому
 * callers могут безопасно вызвать его для --help и invalid configuration.
 */
void usage(const char *name);

/** 
 * @brief Разбирает и валидирует matrix executor CLI.
 * @param argc Число CLI arguments.
 * @param argv CLI argument vector.
 * @param options Указатель на структуру для сохранения разобранных опций.
 * @return BENCH_MATRIX_STATUS_SUCCESS при успешном разборе, 
 *         BENCH_MATRIX_STATUS_HELP при запросе справки,
 *         BENCH_MATRIX_STATUS_ARGUMENT_ERROR при ошибке валидации.
 * @details Алгоритм устанавливает reproducible defaults, обрабатывает option/value
 * pairs, проверяет обязательные binary/manifest paths и требует кратность MT
 * total iterations числу threads до запуска любого child process.
 */
bench_matrix_status_t parse_options(int argc, char **argv, options_t *options);

#endif /* BENCHMARK_MATRIX_CLI_H */
