/**
 *
 *  Wide-screen Hor+ in the engine's camera FOV (camera.c).
 *
 */

#if !defined(_CAMERA_H_INCLUDED_)
#define _CAMERA_H_INCLUDED_

#ifdef __cplusplus
extern "C" {
#endif

// tan(FovX / 2) of the engine's cameras is this much wider than the game's (1: none)
float camera_hor_plus(void);
// GetProcAddress: the export, or a wrapper of it
void *camera_hook_export(const char *name, void *address);
// the engine's ILTClient (cshell's CreateClientShell gets it: the wrapper keeps it), 0 before
uint32_t camera_client_interface(void);

#ifdef __cplusplus
}
#endif

#endif /* _CAMERA_H_INCLUDED_ */
