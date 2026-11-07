/**
 *
 *  What differs between the two programs built from this runtime: Aliens versus Predator 2 (SR-AvP2, AvP2
 *  1.0.9.6's binaries recompiled into x64/<module>) and Primal Hunt (SR-AvP2PH, Primal Hunt 1.0.2.2's binaries
 *  into x64/ph-<module>). The recompiled modules export the same symbols, so everything else is shared;
 *  game_avp2.c or game_ph.c is linked into each.
 *
 */

#if !defined(_GAME_H_INCLUDED_)
#define _GAME_H_INCLUDED_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *title;                  // window title (-windowtitle)
    // the launcher's -rez archives in its order (later ones override earlier ones), NULL-terminated; the ones
    // that don't exist are left out ("custom" is a directory and always added). "avp2:" = from AvP2's install
    // (setting avp2_dir; default the game directory), a warning when it isn't there; "avp2?:" = optional
    const char *const *rez;
    const char *default_config;         // SR-AvP2.cfg written when there is none
    int primal_hunt;                    // Primal Hunt's engine: some structures differ (render3d.cpp)
} game_info;

extern const game_info game;

#ifdef __cplusplus
}
#endif

#endif /* _GAME_H_INCLUDED_ */
