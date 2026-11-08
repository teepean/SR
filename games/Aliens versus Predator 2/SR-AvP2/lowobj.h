/**
 *
 *  A static object the game sees by its address must be below 2 GB. Normally the whole program is; with SR_BLOB
 *  (the program above 4 GB, macOS) it isn't. Such an object is defined as <name>_init, with LOW_SLOT(name) and
 *  #define <name> LOW_OBJ(name): with SR_BLOB it is a copy of <name>_init in low memory (made on first use, always
 *  used instead), otherwise <name>_init itself. low_object: winapi-kernel32.c.
 *
 */

#if !defined(_LOWOBJ_H_INCLUDED_)
#define _LOWOBJ_H_INCLUDED_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *low_object(void **slot, const void *init, size_t size);

#ifdef __cplusplus
}
#endif

#if defined(SR_BLOB) || defined(PTROFS_64BIT)
#define LOW_SLOT(name) static void *name##_low;
#define LOW_OBJ(name) (*(__typeof__(name##_init) *) low_object(&name##_low, &name##_init, sizeof(name##_init)))
#else
#define LOW_SLOT(name)
#define LOW_OBJ(name) (name##_init)
#endif

#endif /* _LOWOBJ_H_INCLUDED_ */
