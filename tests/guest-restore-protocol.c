// SPDX-License-Identifier: GPL-2.0
// Protocol tests need no root privileges, mlock or real pageout.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <errno.h>

static int protocol_fclose(FILE *file);
#define fclose protocol_fclose
#define main benchmark_main
#include "guest-restore-bench.c"
#undef main
#undef fclose

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int pause_close, close_entered, release_close, fail_close;

static int protocol_fclose(FILE *file)
{
    pthread_mutex_lock(&mutex);
    if (pause_close) {
        close_entered = 1;
        pthread_cond_broadcast(&condition);
        while (!release_close)
            pthread_cond_wait(&condition, &mutex);
    }
    pthread_mutex_unlock(&mutex);
    int result = fclose(file);
    if (fail_close) {
        errno = EIO;
        return EOF;
    }
    return result;
}

static void *writer(void *file)
{
    assert(publish(file, "result.done.tmp", "result.done") == 0);
    return NULL;
}

static void command(const char *text)
{
    FILE *file = fopen("command", "w");
    assert(file);
    assert(fputs(text, file) >= 0);
    assert(fclose(file) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2 && chdir(argv[1]) == 0);
    char tag[TAG_BYTES];
    assert(read_command(tag) == 0);
    command("round1\n");
    assert(read_command(tag) == 1 && !strcmp(tag, "round1"));
    assert(access("command", F_OK) != 0);
    for (size_t i = 0; i < 4; ++i) {
        const char *invalid[] = {"", "missing-newline", "../escape\n", "two\nlines\n"};
        command(invalid[i]);
        assert(read_command(tag) == -1);
    }
    char long_tag[TAG_BYTES + 2];
    memset(long_tag, 'a', TAG_BYTES);
    long_tag[TAG_BYTES] = '\n';
    long_tag[TAG_BYTES + 1] = '\0';
    command(long_tag);
    assert(read_command(tag) == -1);

    FILE *file = fopen("result.done.tmp", "w");
    assert(file && fputs("errors=0\n", file) >= 0);
    pause_close = 1;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, writer, file) == 0);
    pthread_mutex_lock(&mutex);
    while (!close_entered)
        pthread_cond_wait(&condition, &mutex);
    // Even when the writer is paused during close, .done is not visible.
    assert(access("result.done", F_OK) != 0);
    assert(access("result.done.tmp", F_OK) == 0);
    release_close = 1;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&mutex);
    assert(pthread_join(thread, NULL) == 0);
    pause_close = 0;
    file = fopen("result.done", "r");
    char line[32];
    assert(file && fgets(line, sizeof(line), file));
    assert(!strcmp(line, "errors=0\n"));
    assert(fclose(file) == 0);
    assert(access("result.done.tmp", F_OK) != 0);

    struct benchmark bench = {0};
    assert(measure_and_publish(&bench, "result") == -1);
    file = fopen("failed.done.tmp", "w");
    assert(file);
    fail_close = 1;
    assert(publish(file, "failed.done.tmp", "failed.done") == -1);
    fail_close = 0;
    assert(access("failed.done", F_OK) != 0);
    assert(access("failed.done.tmp", F_OK) != 0);
    puts("PASS: command validation, atomic completion, duplicate tags and close failure");
    return 0;
}
