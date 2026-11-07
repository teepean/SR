/**
 *
 *  PE images as data (peimage.c): lithtech.exe at its image base, resource-only DLLs, their resources.
 *
 */

#if !defined(_PEIMAGE_H_INCLUDED_)
#define _PEIMAGE_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// maps a PE image (at its image base if at_image_base and possible, else anywhere below 2 GB); NULL on failure
uint8_t *pe_image_load(const char *winpath, int at_image_base);
int pe_image_is_loaded(const void *base);
uint8_t *pe_image_find(const char *name);
// the image containing p (NULL if none)
const uint8_t *pe_image_of(const void *p);

// name/type: MAKEINTRESOURCE values or strings; returns the resource's data entry or NULL
const uint8_t *pe_resource_find(const uint8_t *base, uintptr_t name, uintptr_t type);
void *pe_resource_data(const uint8_t *base, const uint8_t *entry, uint32_t *size);
// RT_STRING: returns the length copied
int pe_load_string(const uint8_t *base, uint32_t id, char *buffer, int size);

#ifdef __cplusplus
}
#endif

#endif /* _PEIMAGE_H_INCLUDED_ */
