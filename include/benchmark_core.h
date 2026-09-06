/**
 * @file benchmark_core.h
 * @brief Публичный C11 adapter API воспроизводимого ST/MT benchmark-core.
 *
 * @details
 * Core отделяет универсальный lifecycle от предметной операции клиента. Алгоритм
 * запуска создаёт deterministic immutable dataset callback-ом initialize, перед
 * каждой измеряемой операцией получает independent mutable copy, вызывает operation
 * и складывает checksum. ST выполняет этот цикл последовательно; MT даёт каждому
 * worker отдельную state область и синхронизирует старт barriers, поэтому core не
 * передаёт один mutable record между потоками.
 */
#ifndef BENCHMARK_CORE_H
#define BENCHMARK_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Представляет именованный boolean result внутреннего и callback контракта.
 * @details
 * Тип устраняет неявные `0`/`1` в управляющих ветвях. Он применяется для состояний
 * ready/released/aborted и predicate results; это не статус ошибки функции.
 */
typedef enum {
    BENCHMARK_BOOLEAN_FALSE = 0, /**< Условие ложно или флаг не установлен. */
    BENCHMARK_BOOLEAN_TRUE = 1   /**< Условие истинно или флаг установлен. */
} benchmark_boolean_t;

/**
 * @brief Описывает все статусы, возвращаемые core lifecycle и его helpers.
 * @details
 * Значения позволяют adapter binary и internal cleanup различать ошибку аргументов,
 * allocation, clock, callback, worker и protocol без неименованных чисел. Public
 * runner возвращает этот enum; ISO C `main` mapping выполняет только project adapter.
 */
typedef enum {
    BENCHMARK_CORE_STATUS_SUCCESS = 0,       /**< Lifecycle завершён и protocol опубликован. */
    BENCHMARK_CORE_STATUS_HELP = 1,          /**< Запрошен `--help`; usage напечатан без benchmark run. */
    BENCHMARK_CORE_STATUS_ARGUMENT_ERROR = 2,/**< CLI, ENV, token или adapter contract некорректен. */
    BENCHMARK_CORE_STATUS_ALLOCATION_ERROR = 3, /**< Dataset/workspace/worker allocation не удался. */
    BENCHMARK_CORE_STATUS_CLOCK_ERROR = 4,   /**< CLOCK_MONOTONIC не предоставил timestamp. */
    BENCHMARK_CORE_STATUS_CALLBACK_ERROR = 5,/**< Adapter initialize/operation вернул неуспешный status. */
    BENCHMARK_CORE_STATUS_THREAD_ERROR = 6   /**< pthread create/join/synchronization lifecycle не удался. */
} benchmark_core_status_t;

/**
 * @brief Описывает result, который domain adapter возвращает core callback-ам.
 * @details
 * Adapter обязан явно сопоставить status предметной библиотеки с этими значениями.
 * Core интерпретирует только SUCCESS; все прочие codes приводят к CALLBACK_ERROR,
 * но сохраняют семантическое имя в project-owned adapter code.
 */
typedef enum {
    BENCHMARK_ADAPTER_STATUS_SUCCESS = 0, /**< Предметная операция завершилась успешно. */
    BENCHMARK_ADAPTER_STATUS_INPUT_ERROR = 1, /**< Workload/state нарушает adapter precondition. */
    BENCHMARK_ADAPTER_STATUS_OPERATION_ERROR = 2 /**< Предметная операция вернула runtime failure. */
} benchmark_adapter_status_t;

/**
 * @brief Описывает workload metadata, передаваемые project-owned adapter без изменений.
 * @details
 * Core разбирает CLI/ENV, печатает metadata в protocol и передаёт один immutable
 * descriptor callbacks. Он намеренно не приписывает предметную семантику строкам:
 * bignum adapter может трактовать operation_kind как shift path, а byte adapter —
 * как transform. seed, warmup и data_count образуют воспроизводимый lifecycle input.
 */
typedef struct {
    const char *data_mode; /**< Legacy mode или `custom`; protocol compatibility field. */
    const char *input_kind; /**< Domain-defined source input class, например `zero`. */
    const char *operation_kind; /**< Domain-defined operation path, например `bit`. */
    const char *measure_mode; /**< Declared timing boundary text: end-to-end/kernel-only. */
    const char *size_profile; /**< Domain-defined logical operand/state length profile. */
    const char *capacity_profile; /**< Domain-defined capacity boundary profile. */
    uint64_t seed; /**< Stable deterministic dataset seed. */
    uint64_t warmup; /**< Unmeasured operation count per ST run or MT worker. */
    size_t data_count; /**< Number of immutable source records in cyclic dataset. */
} benchmark_workload_t;

/**
 * @brief Создаёт один immutable source-state record deterministic dataset.
 * @param state Zeroed writable buffer размера benchmark_adapter_t::state_size.
 * @param sequence_index Стабильный индекс record при равных seed/workload.
 * @param workload Непосредственно переданный immutable workload descriptor.
 * @param adapter_context Project-owned opaque context.
 * @return Именованный benchmark_adapter_status_t result.
 * @details
 * Алгоритм callback-а должен полностью инициализировать `state` без shared mutable
 * state. Core вызывает initialize до warm-up/measurement, сохраняет source dataset
 * и использует memcpy для подготовки каждой in-place operation.
 */
typedef benchmark_adapter_status_t (*benchmark_initialize_fn)(
    void *state,
    uint64_t sequence_index,
    const benchmark_workload_t *workload,
    void *adapter_context);

/**
 * @brief Выполняет одну измеряемую in-place операцию над mutable state record.
 * @param state Независимая mutable копия одного source record.
 * @param iteration Logical ST iteration либо iteration соответствующего MT worker.
 * @param workload Непосредственно переданный immutable workload descriptor.
 * @param adapter_context Project-owned opaque context.
 * @return Именованный benchmark_adapter_status_t result.
 * @details
 * Алгоритм adapter-а не должен хранить result в shared global state. В kernel-only
 * mode core исключает preparation copy из elapsed interval; в end-to-end mode copy
 * входит в interval. Callback выбирает operation parameter детерминированно из
 * iteration/workload, если это необходимо конкретному bignum profile.
 */
typedef benchmark_adapter_status_t (*benchmark_operation_fn)(
    void *state,
    uint64_t iteration,
    const benchmark_workload_t *workload,
    void *adapter_context);

/**
 * @brief Производит deterministic observable checksum post-operation state.
 * @param state Read-only post-operation record.
 * @param iteration Logical iteration соответствующего вызова.
 * @param adapter_context Project-owned opaque context.
 * @return Наблюдаемое 64-bit значение для protocol checksum reduction.
 * @details
 * Алгоритм должен читать достаточную часть результата, чтобы operation не могла
 * быть удалена оптимизатором как ненаблюдаемая. Core смешивает callback values в
 * final checksum, который печатается в machine-readable completion line.
 */
typedef uint64_t (*benchmark_checksum_fn)(
    const void *state,
    uint64_t iteration,
    void *adapter_context);

/**
 * @brief Связывает concrete client operation с generic benchmark-core lifecycle.
 *
 * @details
 * Core валидирует все поля до allocation и запуска. state_size определяет один
 * opaque record; callbacks и success_code определяют предметный contract. Adapter
 * не владеет dataset memory: core создаёт и освобождает buffers, тогда как context
 * остаётся собственностью вызывающего project до возврата run function.
 */
typedef struct {
    const char *benchmark_name; /**< Stable protocol identifier без whitespace/equal sign. */
    size_t state_size; /**< Размер одного opaque mutable state record в bytes. */
    benchmark_adapter_status_t success_code; /**< Domain success code, обычно BENCHMARK_ADAPTER_STATUS_SUCCESS. */
    void *adapter_context; /**< Project-owned opaque context, живущий весь benchmark run. */
    benchmark_initialize_fn initialize; /**< Callback deterministic source-state initialization. */
    benchmark_operation_fn operation; /**< Callback измеряемой in-place операции. */
    benchmark_checksum_fn checksum; /**< Callback наблюдаемого post-operation checksum. */
} benchmark_adapter_t;

/**
 * @brief Запускает generic parameterized single-thread benchmark harness.
 * @param argc Число CLI arguments adapter binary.
 * @param argv CLI argument vector adapter binary.
 * @param adapter Валидный binding project operation к core callbacks.
 * @return Именованный benchmark_core_status_t lifecycle result.
 * @details
 * Алгоритм разбирает CLI и совместимый ENV, создаёт deterministic dataset, выполняет
 * warm-up, измеряет declared iteration count одним worker, публикует ровно одну
 * `benchmark=...` строку и затем `Benchmark finished.`. Legacy data-mode mapping
 * сохраняется для существующих Makefile workflows.
 */
benchmark_core_status_t benchmark_core_run_st(
    int argc,
    char **argv,
    const benchmark_adapter_t *adapter);

/**
 * @brief Запускает generic parameterized multi-thread benchmark harness.
 * @param argc Число CLI arguments adapter binary.
 * @param argv CLI argument vector adapter binary.
 * @param adapter Валидный binding project operation к core callbacks.
 * @return Именованный benchmark_core_status_t lifecycle result.
 * @details
 * Алгоритм требует кратность total iterations threads, создаёт per-worker dataset
 * copies и context, barrier-синхронизирует warm-up/start, собирает elapsed/checksum
 * каждого worker и публикует единый aggregate protocol. Потокобезопасность операции
 * достигается отсутствием shared mutable state между callback invocations.
 */
benchmark_core_status_t benchmark_core_run_mt(
    int argc,
    char **argv,
    const benchmark_adapter_t *adapter);

#ifdef __cplusplus
}
#endif

#endif
