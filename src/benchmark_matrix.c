/**
 * @file benchmark_matrix.c
 * @brief Реализация библиотеки benchmark matrix.
 * @details Содержит внутренние вспомогательные функции и основную логику запуска процессов.
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "benchmark_matrix.h"
#include <json_lib.h>

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** 
 * @brief Копирует и валидирует одно строковое поле manifest profile.
 * @details Алгоритм получает JSON string token, создаёт temporary copy, отвергает
 * whitespace/quotes/equal sign и переполнение fixed destination, защищая exec argv.
 */
static bench_matrix_status_t copy_value(const json_document_t *document, size_t object,
    const char *key, char output[FIELD_SIZE])
{
    size_t token;
    char *text = NULL;
    json_boolean_t is_string;
    json_status_t json_status;
    if (document == NULL || key == NULL || output == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    json_status = json_object_get(document, object, key, &token);
    if (json_status != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    json_status = json_token_has_type(document, token, JSON_TOKEN_STRING, &is_string);
    if (json_status != JSON_STATUS_SUCCESS || is_string != JSON_BOOLEAN_TRUE) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    json_status = json_token_copy_text(document, token, &text);
    if (json_status != JSON_STATUS_SUCCESS) return json_status == JSON_STATUS_ALLOCATION_ERROR
        ? BENCH_MATRIX_STATUS_ALLOCATION_ERROR : BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    if (text[0] == '\0' || strpbrk(text, " \t\r\n=\"") != NULL || strlen(text) >= FIELD_SIZE) {
        (void)json_memory_free(text);
        return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    }
    strcpy(output, text);
    (void)json_memory_free(text);
    return BENCH_MATRIX_STATUS_SUCCESS;
}

/** 
 * @brief Загружает и проверяет versioned JSON manifest profiles.
 * @details Алгоритм требует schema_version 1 и непустой bounded profiles array,
 * копирует все workload dimensions и выполняет O(n²) duplicate-id check для
 * однозначного grouping matrix/statistics artifacts.
 */
static bench_matrix_status_t load_profiles(const char *path,
    profile_t profiles[MAX_PROFILES], size_t *count)
{
    json_document_t document;
    char error[256] = {0};
    size_t array;
    size_t schema_version;
    size_t size = 0U;
    json_boolean_t schema_is_one = JSON_BOOLEAN_FALSE;
    json_status_t json_status;
    if (path == NULL || profiles == NULL || count == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    *count = 0U;
    json_status = json_document_init(&document);
    if (json_status != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    json_status = json_document_load_file(path, &document, error, sizeof(error));
    if (json_status != JSON_STATUS_SUCCESS) {
        fprintf(stderr, "bench_matrix: %s\n", error);
        (void)json_document_destroy(&document);
        return json_status == JSON_STATUS_IO_ERROR ? BENCH_MATRIX_STATUS_IO_ERROR : BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    }
    json_status = json_object_get(&document, 0U, "schema_version", &schema_version);
    if (json_status == JSON_STATUS_SUCCESS) json_status = json_token_equals(&document, schema_version, "1", &schema_is_one);
    if (json_status == JSON_STATUS_SUCCESS) json_status = json_object_get(&document, 0U, "profiles", &array);
    if (json_status == JSON_STATUS_SUCCESS) json_status = json_array_size(&document, array, &size);
    if (json_status != JSON_STATUS_SUCCESS || schema_is_one != JSON_BOOLEAN_TRUE || size == 0U || size > MAX_PROFILES) {
        fprintf(stderr, "bench_matrix: invalid manifest schema\n");
        (void)json_document_destroy(&document);
        return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    }
    for (size_t index = 0U; index < size; ++index) {
        size_t object;
        json_boolean_t is_object = JSON_BOOLEAN_FALSE;
        json_status = json_array_get(&document, array, index, &object);
        if (json_status == JSON_STATUS_SUCCESS) json_status = json_token_has_type(&document, object, JSON_TOKEN_OBJECT, &is_object);
        if (json_status != JSON_STATUS_SUCCESS || is_object != JSON_BOOLEAN_TRUE ||
            copy_value(&document, object, "id", profiles[index].id) != BENCH_MATRIX_STATUS_SUCCESS ||
            copy_value(&document, object, "input_kind", profiles[index].input_kind) != BENCH_MATRIX_STATUS_SUCCESS ||
            copy_value(&document, object, "operation_kind", profiles[index].operation_kind) != BENCH_MATRIX_STATUS_SUCCESS ||
            copy_value(&document, object, "measure_mode", profiles[index].measure_mode) != BENCH_MATRIX_STATUS_SUCCESS ||
            copy_value(&document, object, "size_profile", profiles[index].size_profile) != BENCH_MATRIX_STATUS_SUCCESS ||
            copy_value(&document, object, "capacity_profile", profiles[index].capacity_profile) != BENCH_MATRIX_STATUS_SUCCESS) {
            fprintf(stderr, "bench_matrix: invalid profile %zu\n", index);
            (void)json_document_destroy(&document);
            return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
        }
        for (size_t prior = 0U; prior < index; ++prior) if (strcmp(profiles[prior].id, profiles[index].id) == 0) {
            fprintf(stderr, "bench_matrix: duplicate profile %s\n", profiles[index].id);
            (void)json_document_destroy(&document);
            return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
        }
    }
    *count = size;
    (void)json_document_destroy(&document);
    return BENCH_MATRIX_STATUS_SUCCESS;
}

/** 
 * @brief Запускает benchmark binary, ограничивает ожидание и захватывает output.
 * @details Алгоритм создаёт pipe, fork-ит child, перенаправляет stdout/stderr в
 * pipe и вызывает execv с уже валидированным argv. Parent polls waitpid до timeout,
 * убивает зависший child и сохраняет не более CAPTURE_SIZE bytes для JSON diagnostics.
 */
static bench_matrix_status_t capture_child(char *const command[], double timeout, result_t *result)
{
    int pipes[2];
    pid_t child;
    int status = 0;
    size_t used = 0U;
    if (command == NULL || command[0] == NULL || result == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    result->output = calloc(CAPTURE_SIZE + 1U, 1U);
    if (result->output == NULL) return BENCH_MATRIX_STATUS_ALLOCATION_ERROR;
    if (pipe(pipes) != 0) { free(result->output); result->output = NULL; return BENCH_MATRIX_STATUS_PROCESS_ERROR; }
    /* fork creates an isolated child; no shell is involved in benchmark execution. */
    child = fork();
    if (child == 0) {
        (void)close(pipes[0]);
        (void)dup2(pipes[1], STDOUT_FILENO);
        (void)dup2(pipes[1], STDERR_FILENO);
        (void)close(pipes[1]);
        execv(command[0], command);
        _exit(127);
    }
    if (child < 0) { (void)close(pipes[0]); (void)close(pipes[1]); free(result->output); result->output = NULL; return BENCH_MATRIX_STATUS_PROCESS_ERROR; }
    (void)close(pipes[1]);
    for (double elapsed = 0.0;; elapsed += 0.01) {
        pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        /* A timed-out child cannot hold CI indefinitely; reap it before returning diagnostics. */
        if (waited < 0 || elapsed >= timeout) { (void)kill(child, SIGKILL); (void)waitpid(child, &status, 0); status = -1; break; }
        { const struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000L }; (void)nanosleep(&pause, NULL); }
    }
    while (used < CAPTURE_SIZE) {
        ssize_t received = read(pipes[0], result->output + used, CAPTURE_SIZE - used);
        if (received <= 0) break;
        used += (size_t)received;
    }
    (void)close(pipes[0]);
    result->output[used] = '\0';
    result->returncode = status == -1 ? 124 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128);
    return BENCH_MATRIX_STATUS_SUCCESS;
}

/** 
 * @brief Извлекает один numeric key=value из machine-readable protocol line.
 * @details Алгоритм ищет exact key prefix, применяет strtod и принимает значение
 * только если оно завершается whitespace/end, исключая частичные совпадения ключей.
 */
static bench_matrix_status_t protocol_number(const char *line, const char *key, double *value)
{
    char pattern[64];
    const char *start;
    char *end = NULL;
    if (line == NULL || key == NULL || value == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    (void)snprintf(pattern, sizeof(pattern), "%s=", key);
    start = strstr(line, pattern);
    if (start == NULL) return BENCH_MATRIX_STATUS_PROCESS_ERROR;
    start += strlen(pattern);
    *value = strtod(start, &end);
    return end != start && (*end == ' ' || *end == '\0' || *end == '\n')
        ? BENCH_MATRIX_STATUS_SUCCESS : BENCH_MATRIX_STATUS_PROCESS_ERROR;
}

/** 
 * @brief Проверяет обязательный порядок benchmark completion protocol.
 * @details Алгоритм требует единственную benchmark= line, единственную следующую
 * Benchmark finished. line, извлекает stable benchmark name и оба timing fields.
 * Protocol violation отделяется от non-zero process status в result_t.
 */
static bench_matrix_status_t validate_protocol(result_t *result)
{
    const char *line;
    if (result == NULL || result->output == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    line = strstr(result->output, "benchmark=");
    const char *finish = strstr(result->output, "Benchmark finished.");
    const char *second = line == NULL ? NULL : strstr(line + 10, "benchmark=");
    char name[FIELD_SIZE];
    size_t length;
    /* Exactly one ordered marker pair keeps old Makefile checks and JSON parsing unambiguous. */
    if (line == NULL || second != NULL || finish == NULL || finish < line || strstr(finish + 1, "Benchmark finished.") != NULL) { strcpy(result->error, "invalid benchmark completion protocol"); return BENCH_MATRIX_STATUS_PROCESS_ERROR; }
    length = strcspn(line + 10, " \r\n");
    if (length == 0U || length >= sizeof(name)) { strcpy(result->error, "missing benchmark name"); return BENCH_MATRIX_STATUS_PROCESS_ERROR; }
    memcpy(name, line + 10, length); name[length] = '\0';
    if (protocol_number(line, "elapsed_seconds", &result->elapsed_seconds) != BENCH_MATRIX_STATUS_SUCCESS || protocol_number(line, "ns_per_call", &result->ns_per_call) != BENCH_MATRIX_STATUS_SUCCESS) { strcpy(result->error, "missing timing protocol fields"); return BENCH_MATRIX_STATUS_PROCESS_ERROR; }
    strcpy(result->benchmark, name);
    result->protocol_ok = BENCH_MATRIX_BOOLEAN_TRUE;
    return BENCH_MATRIX_STATUS_SUCCESS;
}

/** 
 * @brief Сериализует validated workload profile в JSON object.
 * @details Алгоритм выводит все шесть dimensions в стабильном порядке и делегирует
 * string escaping json-lib, чтобы manifests/artifacts сохраняли machine readability.
 */
static bench_matrix_status_t json_profile(json_writer_t *writer, const profile_t *profile)
{
    if (writer == NULL || writer->stream == NULL || profile == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    if (json_writer_write_raw(writer, "{\"id\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, profile->id) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"input_kind\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, profile->input_kind) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"operation_kind\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, profile->operation_kind) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"measure_mode\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, profile->measure_mode) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"size_profile\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, profile->size_profile) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"capacity_profile\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, profile->capacity_profile) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, "}") != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_IO_ERROR;
    return BENCH_MATRIX_STATUS_SUCCESS;
}

/** 
 * @brief Сериализует один process result как matrix sample JSON object.
 * @details Алгоритм всегда сохраняет profile/mode/repeat/returncode/stdout, затем
 * добавляет parsed protocol только для valid completion либо protocol_error иначе.
 * Это сохраняет diagnosability неуспешных child runs.
 */
static bench_matrix_status_t json_sample(json_writer_t *writer, const result_t *result,
    const profile_t *profile, const char *mode, uint64_t repeat)
{
    char numeric[192];
    int written;
    if (writer == NULL || writer->stream == NULL || result == NULL || profile == NULL || mode == NULL) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    written = snprintf(numeric, sizeof(numeric), ",\"repeat_index\":%" PRIu64 ",\"returncode\":%d,\"stdout\":", repeat, result->returncode);
    if (written < 0 || (size_t)written >= sizeof(numeric)) return BENCH_MATRIX_STATUS_IO_ERROR;
    if (json_writer_write_raw(writer, "{\"profile_id\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, profile->id) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"mode\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, mode) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, numeric) != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, result->output == NULL ? "" : result->output) != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_IO_ERROR;
    if (result->protocol_ok == BENCH_MATRIX_BOOLEAN_TRUE) {
        written = snprintf(numeric, sizeof(numeric), ",\"elapsed_seconds\":%.9f,\"ns_per_call\":%.9f}", result->elapsed_seconds, result->ns_per_call);
        if (written < 0 || (size_t)written >= sizeof(numeric) ||
            json_writer_write_raw(writer, ",\"protocol\":{\"benchmark\":") != JSON_STATUS_SUCCESS ||
            json_writer_write_string(writer, result->benchmark) != JSON_STATUS_SUCCESS ||
            json_writer_write_raw(writer, numeric) != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_IO_ERROR;
    } else if (json_writer_write_raw(writer, ",\"protocol_error\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, result->error) != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_IO_ERROR;
    return json_writer_write_raw(writer, "}") == JSON_STATUS_SUCCESS
        ? BENCH_MATRIX_STATUS_SUCCESS : BENCH_MATRIX_STATUS_IO_ERROR;
}

/** 
 * @brief Сериализует runtime host metadata в JSON object.
 * @details Алгоритм получает uname, logical CPU count и current scheduler affinity;
 * отсутствующая affinity не является ошибкой и представляется пустым array.
 */
static bench_matrix_status_t json_host(json_writer_t *writer)
{
    struct utsname name;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t set;
    char numeric[64];
    int written;
    if (writer == NULL || writer->stream == NULL || uname(&name) != 0) return BENCH_MATRIX_STATUS_IO_ERROR;
    written = snprintf(numeric, sizeof(numeric), ",\"logical_cpu_count\":%ld,\"cpu_affinity\":[", cpus);
    if (written < 0 || (size_t)written >= sizeof(numeric) ||
        json_writer_write_raw(writer, "{\"tool\":\"bench_matrix-c11\",\"system\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, name.sysname) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"release\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, name.release) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, ",\"machine\":") != JSON_STATUS_SUCCESS ||
        json_writer_write_string(writer, name.machine) != JSON_STATUS_SUCCESS ||
        json_writer_write_raw(writer, numeric) != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_IO_ERROR;
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        int first = 1;
        for (int index = 0; index < CPU_SETSIZE; ++index) if (CPU_ISSET(index, &set)) {
            written = snprintf(numeric, sizeof(numeric), "%s%d", first ? "" : ",", index);
            if (written < 0 || (size_t)written >= sizeof(numeric) ||
                json_writer_write_raw(writer, numeric) != JSON_STATUS_SUCCESS) return BENCH_MATRIX_STATUS_IO_ERROR;
            first = 0;
        }
    }
    return json_writer_write_raw(writer, "]}") == JSON_STATUS_SUCCESS
        ? BENCH_MATRIX_STATUS_SUCCESS : BENCH_MATRIX_STATUS_IO_ERROR;
}

bench_matrix_status_t bench_matrix_execute(const options_t *options, int *failures, uint64_t *sample_count)
{
    profile_t profiles[MAX_PROFILES];
    size_t profile_count;
    char error[256] = {0};
    char fragment[256];
    json_writer_t writer = {0};
    json_status_t json_status;
    
    if (!options || !failures || !sample_count) return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    
    *failures = 0;
    *sample_count = 0U;

    if (load_profiles(options->manifest, profiles, &profile_count) != BENCH_MATRIX_STATUS_SUCCESS ||
        access(options->st_binary, X_OK) != 0 || access(options->mt_binary, X_OK) != 0) {
        return BENCH_MATRIX_STATUS_ARGUMENT_ERROR;
    }
    
    json_status = json_writer_open(options->output, &writer, error, sizeof(error));
    if (json_status != JSON_STATUS_SUCCESS) { fprintf(stderr, "bench_matrix: %s\n", error); return BENCH_MATRIX_STATUS_IO_ERROR; }
    
    if (json_writer_write_raw(&writer, "{\"schema_version\":1,\"host\":") != JSON_STATUS_SUCCESS ||
        json_host(&writer) != BENCH_MATRIX_STATUS_SUCCESS ||
        snprintf(fragment, sizeof(fragment), ",\"configuration\":{\"repetitions\":%" PRIu64 ",\"iterations\":%" PRIu64 ",\"mt_total_iterations\":%" PRIu64 ",\"threads\":%zu,\"warmup\":%" PRIu64 ",\"data_count\":%" PRIu64 ",\"seed\":%" PRIu64 "},\"profiles\":[", options->repetitions, options->iterations, options->mt_total_iterations, options->threads, options->warmup, options->data_count, options->seed) < 0 ||
        json_writer_write_raw(&writer, fragment) != JSON_STATUS_SUCCESS) {
        (void)json_writer_abort(&writer);
        return BENCH_MATRIX_STATUS_IO_ERROR;
    }
    
    for (size_t index = 0U; index < profile_count; ++index) {
        if ((index != 0U && json_writer_write_raw(&writer, ",") != JSON_STATUS_SUCCESS) ||
            json_profile(&writer, &profiles[index]) != BENCH_MATRIX_STATUS_SUCCESS) {
            (void)json_writer_abort(&writer);
            return BENCH_MATRIX_STATUS_IO_ERROR;
        }
    }
    
    if (json_writer_write_raw(&writer, "],\"samples\":[") != JSON_STATUS_SUCCESS) {
        (void)json_writer_abort(&writer);
        return BENCH_MATRIX_STATUS_IO_ERROR;
    }
    
    for (size_t profile_index = 0U; profile_index < profile_count; ++profile_index) {
        for (int mode_index = 0; mode_index < 2; ++mode_index) {
            for (uint64_t repeat = 0U; repeat < options->repetitions; ++repeat) {
                char iterations[32], total[32], threads[32], warmup[32], data_count[32], seed[32];
                const char *mode = mode_index == 0 ? "st" : "mt";
                const char *binary = mode_index == 0 ? options->st_binary : options->mt_binary;
                result_t result = {0};
                
                (void)snprintf(iterations, sizeof(iterations), "%" PRIu64, options->iterations);
                (void)snprintf(total, sizeof(total), "%" PRIu64, options->mt_total_iterations);
                (void)snprintf(threads, sizeof(threads), "%zu", options->threads);
                (void)snprintf(warmup, sizeof(warmup), "%" PRIu64, options->warmup);
                (void)snprintf(data_count, sizeof(data_count), "%" PRIu64, options->data_count);
                (void)snprintf(seed, sizeof(seed), "%" PRIu64, options->seed);
                
                char *command_st[] = { (char *)binary, "--iterations", iterations, "--warmup", warmup, "--data-count", data_count, "--seed", seed, "--input-kind", profiles[profile_index].input_kind, "--operation-kind", profiles[profile_index].operation_kind, "--measure-mode", profiles[profile_index].measure_mode, "--size-profile", profiles[profile_index].size_profile, "--capacity-profile", profiles[profile_index].capacity_profile, NULL };
                char *command_mt[] = { (char *)binary, "--threads", threads, "--total-iterations", total, "--warmup", warmup, "--data-count", data_count, "--seed", seed, "--input-kind", profiles[profile_index].input_kind, "--operation-kind", profiles[profile_index].operation_kind, "--measure-mode", profiles[profile_index].measure_mode, "--size-profile", profiles[profile_index].size_profile, "--capacity-profile", profiles[profile_index].capacity_profile, NULL };
                char *const *command = mode_index == 0 ? command_st : command_mt;
                
                /* Each profile/mode/repeat receives a fresh process and a fully explicit argv. */
                if (capture_child(command, options->timeout_seconds, &result) != BENCH_MATRIX_STATUS_SUCCESS) { 
                    result.returncode = 125; 
                    strcpy(result.error, "cannot execute benchmark"); 
                }
                if (result.returncode != 0 && result.error[0] == '\0') {
                    strcpy(result.error, "benchmark returned non-zero status");
                }
                if (result.returncode == 0) {
                    (void)validate_protocol(&result);
                }
                
                if ((*sample_count != 0U && json_writer_write_raw(&writer, ",") != JSON_STATUS_SUCCESS) ||
                    json_sample(&writer, &result, &profiles[profile_index], mode, repeat) != BENCH_MATRIX_STATUS_SUCCESS) {
                    free(result.output);
                    (void)json_writer_abort(&writer);
                    return BENCH_MATRIX_STATUS_IO_ERROR;
                }
                
                ++(*sample_count);
                if (result.protocol_ok != BENCH_MATRIX_BOOLEAN_TRUE) {
                    ++(*failures);
                } else {
                    printf("%s %s repeat=%" PRIu64 "/%" PRIu64 " ns_per_call=%.3f\n", 
                           profiles[profile_index].id, mode, repeat + 1U, options->repetitions, result.ns_per_call);
                }
                free(result.output);
            }
        }
    }
    
    if (snprintf(fragment, sizeof(fragment), "],\"failures\":%d}\n", *failures) < 0 ||
        json_writer_write_raw(&writer, fragment) != JSON_STATUS_SUCCESS ||
        json_writer_commit(&writer, error, sizeof(error)) != JSON_STATUS_SUCCESS) {
        fprintf(stderr, "bench_matrix: %s\n", error);
        (void)json_writer_abort(&writer);
        return BENCH_MATRIX_STATUS_IO_ERROR;
    }
    
    return BENCH_MATRIX_STATUS_SUCCESS;
}
