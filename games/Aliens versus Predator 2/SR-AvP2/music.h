/**
 *
 *  Music: the engine's DirectMusic manager replaced (music.c).
 *
 */

#if !defined(_MUSIC_H_INCLUDED_)
#define _MUSIC_H_INCLUDED_

#ifdef __cplusplus
extern "C" {
#endif

// points the engine's CLTDirectMusicMgr vtable at the replacement (before the engine starts)
void music_install(void);

#ifdef __cplusplus
}
#endif

#endif /* _MUSIC_H_INCLUDED_ */
