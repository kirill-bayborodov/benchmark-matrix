/**
 * @file benchmark_core_smoke.c
 * @brief Deterministic neutral adapter для smoke-проверки benchmark-core.
 * @details
 * Test adapter моделирует in-place byte transform без domain dependency. Он
 * преобразует workload dimensions в predictable state size/input/operation и
 * вызывает public ST либо MT entry point. Успешный запуск одновременно проверяет
 * dataset lifecycle, kernel/end-to-end semantics и ordered completion protocol.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "benchmark_core.h"

/**
 * @brief Хранит mutable byte-buffer state одного test dataset record.
 * @details
 * bytes имеет фиксированную максимальную capacity, а used задаёт active prefix.
 * Это позволяет проверить tiny/medium/large/near-capacity profiles без allocation
 * внутри adapter callbacks и без shared mutable memory между MT workers.
 */
typedef struct {
    uint8_t bytes[64];
    size_t used;
} byte_state_t;

/**
 * @brief Производит deterministic pseudo-random 64-bit sequence step.
 * @details
 * Алгоритм применяет три xor-shift transform. Он нужен только для repeatable test
 * data и не является cryptographic generator; равные seed/index всегда дают
 * равный byte sequence для dataset fingerprint regression checks.
 */
static uint64_t next_value(uint64_t value)
{
    value ^= value << 7U;
    value ^= value >> 9U;
    value ^= value << 8U;
    return value;
}

/**
 * @brief Отображает generic size/capacity profile в active byte count.
 * @details
 * Алгоритм задаёт canonical lengths для named profiles. near-capacity побеждает
 * size_profile и выбирает полный 64-byte buffer, проверяя boundary path adapter-а.
 */
static size_t select_size(const benchmark_workload_t *workload)
{
    if (strcmp(workload->size_profile, "tiny") == 0) return 1U;
    if (strcmp(workload->size_profile, "small") == 0) return 8U;
    if (strcmp(workload->size_profile, "medium") == 0) return 32U;
    if (strcmp(workload->size_profile, "large") == 0 ||
        strcmp(workload->size_profile, "near-capacity") == 0 ||
        strcmp(workload->capacity_profile, "near-capacity") == 0) return 64U;
    return 16U;
}

/**
 * @brief Инициализирует deterministic immutable byte-state source record.
 * @details
 * Алгоритм zeroes complete record, выбирает active size и определяет zero input
 * по input_kind/sequence parity. Nonzero path заполняет prefix xorshift values и
 * заменяет accidental zero bytes, поэтому test operation имеет observable input.
 */
static benchmark_adapter_status_t initialize_byte_state(
    void *opaque,
    uint64_t sequence_index,
    const benchmark_workload_t *workload,
    void *context)
{
    byte_state_t *state = opaque;
    uint64_t value = workload->seed ^ sequence_index;
    const int zero = strcmp(workload->input_kind, "zero") == 0 ||
        (strcmp(workload->input_kind, "mixed") == 0 && (sequence_index % 2U) == 0U);

    (void)context;
    memset(state, 0, sizeof(*state));
    state->used = select_size(workload);
    if (zero) return BENCHMARK_ADAPTER_STATUS_SUCCESS;
    for (size_t index = 0U; index < state->used; ++index) {
        value = next_value(value);
        state->bytes[index] = (uint8_t)value;
        /* Preserve a visible nonzero source value even when generator's low byte is zero. */
        if (state->bytes[index] == 0U) state->bytes[index] = UINT8_C(1);
    }
    return BENCHMARK_ADAPTER_STATUS_SUCCESS;
}

/**
 * @brief Выполняет declared noop/xor/rotate in-place byte transform.
 * @details
 * Алгоритм immediately returns for noop; иначе проходит active prefix и выбирает
 * rotate-left или iteration-dependent xor. State принадлежит только текущему core
 * workspace, поэтому callback безопасен для parallel MT invocations.
 */
static benchmark_adapter_status_t transform_byte_state(
    void *opaque,
    uint64_t iteration,
    const benchmark_workload_t *workload,
    void *context)
{
    byte_state_t *state = opaque;

    (void)context;
    if (strcmp(workload->operation_kind, "noop") == 0) return BENCHMARK_ADAPTER_STATUS_SUCCESS;
    for (size_t index = 0U; index < state->used; ++index) {
        const uint8_t mask = (uint8_t)(iteration + index + 1U);
        if (strcmp(workload->operation_kind, "rotate") == 0) {
            state->bytes[index] = (uint8_t)((state->bytes[index] << 1U) |
                (state->bytes[index] >> 7U));
        } else {
            state->bytes[index] ^= mask;
        }
    }
    return BENCHMARK_ADAPTER_STATUS_SUCCESS;
}

/**
 * @brief Смешивает transformed active bytes в observable 64-bit checksum.
 * @details
 * Алгоритм выполняет FNV-style recurrence по active prefix и iteration, делая
 * operation result observable для optimizer и для core completion protocol.
 */
static uint64_t checksum_byte_state(const void *opaque, uint64_t iteration, void *context)
{
    const byte_state_t *state = opaque;
    uint64_t hash = UINT64_C(1469598103934665603) ^ iteration;

    (void)context;
    for (size_t index = 0U; index < state->used; ++index) {
        hash ^= state->bytes[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash ^ (uint64_t)state->used;
}

/**
 * @brief Создаёт test adapter и dispatch-ит ST либо MT public runner.
 * @param argc Число CLI arguments.
 * @param argv CLI argument vector.
 * @return Код, возвращённый benchmark_core_run_st или benchmark_core_run_mt.
 * @details
 * Алгоритм собирает static adapter binding и сканирует CLI на `--threads`. Наличие
 * option выбирает MT runner; все остальные CLI semantics валидирует сам core.
 */
int main(int argc, char **argv)
{
    const benchmark_adapter_t adapter = {
        .benchmark_name = "benchmark_core_smoke",
        .state_size = sizeof(byte_state_t),
        .success_code = BENCHMARK_ADAPTER_STATUS_SUCCESS,
        .adapter_context = NULL,
        .initialize = initialize_byte_state,
        .operation = transform_byte_state,
        .checksum = checksum_byte_state
    };

    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--threads") == 0) {
            return benchmark_core_run_mt(argc, argv, &adapter) == BENCHMARK_CORE_STATUS_SUCCESS ? 0 : 1;
        }
    }
    return benchmark_core_run_st(argc, argv, &adapter) == BENCHMARK_CORE_STATUS_SUCCESS ? 0 : 1;
}
