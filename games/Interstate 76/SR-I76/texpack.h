/**
 *
 *  Texture packs (texpack.c): replacement images and texture dumps.
 *
 */

#if !defined(_TEXPACK_H_INCLUDED_)
#define _TEXPACK_H_INCLUDED_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void texpack_init(void);
// rgba = the game's texture (w x h, RGBA bytes); returns a replacement (RGBA bytes, *out_w x *out_h) or NULL
const uint32_t *texpack_lookup(int w, int h, const uint32_t *rgba, int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* _TEXPACK_H_INCLUDED_ */
