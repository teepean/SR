/**
 *
 *  Win32 API emulation - shared definitions.
 *
 */

#if !defined(_WINAPI_H_INCLUDED_)
#define _WINAPI_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define INVALID_HANDLE_VALUE ((void *)(uintptr_t)0xffffffff)

#define ERROR_FILE_NOT_FOUND 2
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_WRITE_FAULT 29
#define ERROR_READ_FAULT 30
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_ALREADY_EXISTS 183

// set from environment variable I76_DEBUG
extern int winapi_debug;

void winapi_set_last_error(uint32_t err);
uint32_t winapi_get_ticks(void);

// processes pending SDL events (window messages, input)
void winapi_process_events(void);

// winapi-kernel32.c: memory the runtime hands to the game (protected emulated heap, zeroed, below 2 GB)
void *game_malloc(uint32_t size);
void *game_calloc(uint32_t n, uint32_t size);
void game_free(void *p);

// winapi-kernel32.c: checks all heap blocks for overruns (I76_HEAPCHECK=1)
void heap_check_all(void);

// c2asm.c: calls recompiled game code (cdecl or stdcall) with 32-bit arguments
uint32_t call_game(uint32_t func, uint32_t nargs, const uint32_t *args);

// joystick.c
void joystick_startup(void);
void joystick_script(const char *cmd, int a, int b);

#ifdef __cplusplus
}
#endif

#endif /* _WINAPI_H_INCLUDED_ */
