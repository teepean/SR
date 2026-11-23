#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef int (*binop)(int, int);

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int op_div(int a, int b) { return b ? a / b : 0; }

/* function pointer table in data (relocations) */
static binop ops[4] = { op_add, op_sub, op_mul, op_div };
static const char *op_names[4] = { "add", "sub", "mul", "div" };

int counters[64];          /* bss */
int primes[8] = { 2, 3, 5, 7, 11, 13, 17, 19 }; /* data */
static char buffer[256];

__attribute__((noinline)) static const char *classify(int v)
{
    switch (v)              /* dense switch -> jump table */
    {
        case 0: return "zero";
        case 1: return "one";
        case 2: return "two";
        case 3: return "three";
        case 4: return "four";
        case 5: return "five";
        case 6: return "six";
        case 7: return "seven";
        case 8: return "eight";
        default: return "many";
    }
}

__attribute__((noinline)) static int sparse(int v)
{
    switch (v)
    {
        case 10: return 1;
        case 11: return 4;
        case 13: return 9;
        case 14: return 16;
        case 17: return 25;
        case 19: return 36;
        case 20: return 49;
        case 1000: return 100;
        default: return -1;
    }
}

static int cmp_int(const void *a, const void *b)  /* leaf callback */
{
    return *(const int *)a - *(const int *)b;
}

__attribute__((noinline)) static long long fib(int n)
{
    return (n < 2) ? n : fib(n - 1) + fib(n - 2);
}

__attribute__((noinline)) static double poly(double x)
{
    return 3.25 * x * x - 1.5 * x + 0.125 + sqrt(x);
}

int main(int argc, char **argv)
{
    int i, arr[10] = { 9, 3, 7, 1, 8, 2, 6, 0, 5, 4 };
    unsigned long long h = 1469598103934665603ULL;

    for (i = 0; i < 4; i++)
        printf("%s(%d,%d) = %d\n", op_names[i], 12, 4, ops[i](12, 4));

    for (i = 0; i < 11; i++)
        printf("classify(%d) = %s, sparse(%d) = %d\n", i, classify(i), i + 10, sparse(i + 10));
    printf("sparse(1000) = %d\n", sparse(1000));

    qsort(arr, 10, sizeof(int), cmp_int);
    for (i = 0; i < 10; i++) printf("%d ", arr[i]);
    printf("\n");

    for (i = 0; i < 64; i++) counters[i] = i * primes[i & 7];
    for (i = 0; i < 64; i++) h = (h ^ (unsigned)counters[i]) * 1099511628211ULL;
    printf("hash = %016llx\n", h);

    printf("fib(30) = %lld\n", fib(30));
    printf("poly(2.0) = %.6f, poly(7.5) = %.6f\n", poly(2.0), poly(7.5));

    snprintf(buffer, sizeof(buffer), "%s-%d-%s", "buf", argc, classify(argc));
    printf("buffer = %s (len %d)\n", buffer, (int)strlen(buffer));
    return 0;
}
