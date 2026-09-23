/**
 *
 *  Settings: SR-I76.cfg in the game directory ("key = value"), overridden by environment variables
 *  I76_<KEY> (upper case).
 *
 */

#if !defined(_CONFIG_H_INCLUDED_)
#define _CONFIG_H_INCLUDED_

#ifdef __cplusplus
extern "C" {
#endif

void config_load(void);
// value of a setting (environment first, then the file), NULL if not set
const char *config_get(const char *key);
int config_get_int(const char *key, int def);

#ifdef __cplusplus
}
#endif

#endif /* _CONFIG_H_INCLUDED_ */
