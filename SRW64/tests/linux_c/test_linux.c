/*
 * Test program for recompilation of Linux executables (statically linked, not position independent):
 * jump tables, function pointers in data and code, callbacks, varargs, floating point,
 * thread local storage, threads, setjmp/longjmp, atexit handlers, glibc string functions (IFUNC)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <pthread.h>
#include <errno.h>

static __thread int tls_counter = 5;
static __thread char tls_buffer[64];

typedef int (*operation)(int, int);

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_div(int a, int b) { return (b != 0) ? a / b : 0; }

static const struct {
    const char *name;
    operation func;
} operations[] = {
    { "add", op_add },
    { "sub", op_sub },
    { "mul", op_mul },
    { "div", op_div },
};

static jmp_buf jump_buffer;

__attribute__((noinline)) static const char *classify(int value)
{
    switch (value)
    {
        case 0: return "zero";
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 5: return "five";
        case 7: return "seven";
        case 8: return "eight";
        case 11: return "eleven";
        case 13: return "thirteen";
        default: return "other";
    }
}

__attribute__((noinline)) static int compute(int selector, int x)
{
    switch (selector)
    {
        case 10: x = x * 3 + 1; break;
        case 11: x = x / 2; break;
        case 12: x ^= 0x55; break;
        case 13: x = -x; break;
        case 14: x = x << 2; break;
        case 15: x = x % 7; break;
        case 16: x += 100; break;
        default: x = 0; break;
    }
    return x;
}

static int compare_ints(const void *p1, const void *p2)
{
    int a = *(const int *)p1, b = *(const int *)p2;
    return (a > b) - (a < b);
}

static double sum_values(int count, ...)
{
    va_list ap;
    double sum = 0;
    int index;

    va_start(ap, count);
    for (index = 0; index < count; index++) sum += va_arg(ap, double);
    va_end(ap);
    return sum;
}

__attribute__((noinline)) static void do_longjmp(int value)
{
    longjmp(jump_buffer, value);
}

static void *thread_function(void *arg)
{
    int index = (int)(long)arg;
    int sum = 0, i;

    tls_counter += index;
    snprintf(tls_buffer, sizeof(tls_buffer), "thread %d", index);
    for (i = 0; i < 1000; i++) sum += compute(10 + (i % 8), i);
    return (void *)(long)(sum + tls_counter + (int)strlen(tls_buffer));
}

static void exit_handler1(void) { printf("exit handler 1\n"); }
static void exit_handler2(void) { printf("exit handler 2\n"); }

int main(int argc, char *argv[])
{
    int values[] = { 42, -7, 13, 0, 99, 5, 23, -100, 64, 8 };
    char text[256];
    pthread_t threads[4];
    int index, result;
    double angle;

    atexit(exit_handler1);
    atexit(exit_handler2);

    printf("argc: %d\n", argc);

    for (index = 0; index < 15; index++) printf("%d: %s\n", index, classify(index));
    for (index = 9; index < 18; index++) printf("compute(%d): %d\n", index, compute(index, 37));

    for (index = 0; index < (int)(sizeof(operations) / sizeof(operations[0])); index++)
    {
        printf("%s(20, 6) = %d\n", operations[index].name, operations[index].func(20, 6));
    }

    qsort(values, sizeof(values) / sizeof(values[0]), sizeof(values[0]), compare_ints);
    for (index = 0; index < (int)(sizeof(values) / sizeof(values[0])); index++) printf("%d ", values[index]);
    printf("\n");

    printf("varargs sum: %.3f\n", sum_values(4, 1.5, 2.25, 3.125, 4.0));
    for (angle = 0; angle < 3.2; angle += 0.8) printf("sin(%.1f) = %.6f, sqrt = %.6f\n", angle, sin(angle), sqrt(angle));

    strcpy(text, "The quick brown fox");
    strcat(text, " jumps over the lazy dog");
    printf("%s (%d) %s %d\n", text, (int)strlen(text), strstr(text, "lazy"), (int)(strchr(text, 'j') - text));
    memmove(text + 4, text, 20);
    text[24] = 0;
    printf("%s %d\n", text, memcmp(text, "The The", 7));

    result = setjmp(jump_buffer);
    if (result == 0)
    {
        do_longjmp(17);
        printf("not reached\n");
    }
    printf("setjmp returned %d\n", result);

    errno = 0;
    if (fopen("/nonexistent/file", "r") == NULL) printf("fopen failed: %s\n", strerror(errno));

    printf("tls_counter in main: %d\n", tls_counter);
    for (index = 0; index < 4; index++) pthread_create(&threads[index], NULL, thread_function, (void *)(long)(index + 1));
    for (index = 0; index < 4; index++)
    {
        void *ret;
        pthread_join(threads[index], &ret);
        printf("thread %d returned %ld\n", index + 1, (long)ret);
    }
    printf("tls_counter in main: %d\n", tls_counter);

    return 3;
}
