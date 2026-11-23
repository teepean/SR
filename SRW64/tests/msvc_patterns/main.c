/* freestanding test program: only kernel32 imports (GetStdHandle, WriteFile, ExitProcess) */
#include <windows.h>

int rva_switch(int x);
int rva_byte_switch(unsigned int x);
int rva_array_sum(void);

static const char *names[] = { "zero", "one", "two", "three" };
static int (*funcs[3])(int) = { rva_switch, 0, 0 };
static char buffer[64];

static unsigned int str_len(const char *s) { unsigned int n = 0; while (s[n]) n++; return n; }

static void out(const char *s)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, str_len(s), &written, NULL);
}

static void out_int(int v)
{
    char tmp[16];
    int pos = 15, neg = v < 0;
    unsigned int u = neg ? -(unsigned int)v : (unsigned int)v;
    tmp[pos] = 0;
    do { tmp[--pos] = '0' + u % 10; u /= 10; } while (u);
    if (neg) tmp[--pos] = '-';
    out(&tmp[pos]);
}

__attribute__((noinline)) static int gcc_switch(int v)
{
    switch (v)
    {
        case 0: return 7;
        case 1: return 11;
        case 2: return 13;
        case 3: return 17;
        case 4: return 19;
        case 5: return 23;
        case 6: return 29;
        default: return 0;
    }
}

void __stdcall start(void)
{
    int i;

    for (i = -1; i <= 7; i++) { out("rva_switch("); out_int(i); out(") = "); out_int(funcs[0](i)); out("\n"); }
    for (i = 0; i <= 16; i++) { out("rva_byte_switch("); out_int(i); out(") = "); out_int(rva_byte_switch(i)); out("\n"); }
    out("rva_array_sum() = "); out_int(rva_array_sum()); out("\n");
    for (i = 0; i <= 7; i++) { out("gcc_switch("); out_int(i); out(") = "); out_int(gcc_switch(i)); out("\n"); }
    for (i = 0; i < 4; i++) { buffer[i] = names[i][0]; }
    out("buffer = "); out(buffer); out("\n");
    ExitProcess(0);
}
