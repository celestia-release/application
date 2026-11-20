#ifndef APPLICATION_PACKAGES_TABLET_APP_REGISTRY_H
#define APPLICATION_PACKAGES_TABLET_APP_REGISTRY_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int index;
    char app_id[64];
    char display_name[64];
    char action[64];
    char install_path[256];
    char entry_so[256];
    char entry_symbol[64];
    char exec[256];      /* 进程型应用：install_path 下可执行（脚本），点击 fork 执行 */
    /* 桌面图标描述（app.json 可选）：图形类型、图形名、渐变上/下色 #RRGGBB */
    char icon_kind[16];
    char icon_glyph[24];
    char icon_color[16];
    char icon_color2[16];
    int enabled;
} tablet_app_desc_t;

/* Reload installed apps state from runtime config. */
void tablet_app_registry_refresh(void);

/* Scan APPS_BASE_DIR and APPS_SD_BASE_DIR (if defined); init defaults if list missing. */
void tablet_app_registry_scan_installed(void);

/* Runtime update one app switch and persist config file. */
int tablet_app_registry_set_enabled_by_index(int index, int enabled);
int tablet_app_registry_save(void);

/* Enumerate app list for settings/app-manager UI. */
const tablet_app_desc_t *tablet_app_registry_list(int *count);

#ifdef __cplusplus
}
#endif

#endif /* APPLICATION_PACKAGES_TABLET_APP_REGISTRY_H */
