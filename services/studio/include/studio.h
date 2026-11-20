/*
 * studio.h - public interface of the Celestia Studio device service
 *
 * Loaded by app_main as a "so" module (settings.json modules entry):
 *   module_init(config_path) -> 0
 *   module_start()           -> 0  (spawns threads, non-blocking)
 *
 * Serves the PC sync tool on TCP 7777 (see docs/design/nova/anypi_nova_pc_studio.md).
 */
#ifndef STUDIO_H
#define STUDIO_H

#define STUDIO_VERSION "0.1.0"

#ifdef __cplusplus
extern "C"
{
#endif

int module_init(const char *config_path);
int module_start(void);
void module_exit(void);

#ifdef __cplusplus
}
#endif

#endif /* STUDIO_H */
