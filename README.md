# benchmark-core

`benchmark-core` — доменно-независимое C11-ядро для воспроизводимых параметризованных benchmark-runner-ов. Репозиторий может использоваться самостоятельно и подключается потребляющими framework-репозиториями как Git submodule.

## Гарантии ядра

Ядро предоставляет общий lifecycle для однопоточного и многопоточного измерения: детерминированную генерацию immutable dataset через callback, warm-up, `end-to-end` и `kernel-only` boundaries, independent mutable state на каждый вызов, синхронизацию MT worker-ов и aggregation checksum. Оно не зависит от bignum, SIMD-библиотек или конкретного формата данных.

Успешный run всегда заканчивается строками:

```text
benchmark=<stable-name>_st|mt ...
Benchmark finished.
```

Первая строка содержит workload metadata, seed, warmup, data count, fingerprint, checksum, elapsed seconds и nanoseconds per call. Вторая обязана следовать после первой и является единым маркером успешного завершения для внешней автоматизации.

## Adapter API

Клиент передаёт `benchmark_adapter_t` из `include/benchmark_core.h`. Он определяет размер одного state record, callbacks `initialize`, `operation`, `checksum`, имя benchmark и код успеха. Workload поля `input_kind`, `operation_kind`, `size_profile` и `capacity_profile` передаются adapter-у без предметной интерпретации.

## CLI и ENV

ST runner поддерживает `--iterations`, а MT runner — `--threads` и `--total-iterations`; последнее значение обязано делиться на число threads. Оба поддерживают `--data-mode`, `--input-kind`, `--operation-kind`, `--measure-mode`, `--size-profile`, `--capacity-profile`, `--warmup`, `--data-count` и `--seed`. ENV equivalents используют префикс `BENCH_`: например, `BENCH_ITERATIONS`, `BENCH_MT_TOTAL_ITERATIONS`, `BENCH_OPERATION_KIND` и `BENCH_SIZE_PROFILE`.

## Проверка

```bash
make test
make lint
make test_sanitize
make clean && make test_helgrind
```

Smoke-тест использует byte-buffer adapter и проверяет ST, MT, `kernel-only`, mixed inputs, operation kinds и near-capacity metadata. `test_sanitize` запускает AddressSanitizer и UndefinedBehaviorSanitizer; `test_helgrind` проверяет MT lifecycle на data races.
