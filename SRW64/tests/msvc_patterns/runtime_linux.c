/* minimal Linux implementation of the Windows functions used by the recompiled test program */
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define MSABI __attribute__((ms_abi))

MSABI void *GetStdHandle(uint32_t handle)
{
    return (void *)(uintptr_t)((handle == (uint32_t)-11) ? 1 : 2);
}

MSABI int WriteFile(void *handle, const void *buffer, uint32_t length, uint32_t *written, void *overlapped)
{
    ssize_t result = write((int)(uintptr_t)handle, buffer, length);
    if (written != NULL) *written = (result < 0) ? 0 : (uint32_t)result;
    return (result >= 0) ? 1 : 0;
}

MSABI void ExitProcess(uint32_t code)
{
    exit((int)code);
}

extern MSABI void srw_entry(void);

int main(void)
{
    srw_entry();
    return 0;
}
