// SPDX-License-Identifier: Apache-2.0
/*
 * Copyright (C) 2026 Guangzhou Anyka Microelectronics Co., Ltd.
 *
 * Nova boot trim keeps only LV_FONT_MONTSERRAT_28/48 in lv_conf. Plugin apps on
 * /mnt/install may still be linked against the old fuller font set and fail
 * dlopen with "lv_font_montserrat_XX: symbol not found".
 *
 * Export aliases (no extra glyph bitmaps) so those SOs relocate against the
 * nearest retained font. Rebuild/redeploy of app SOs remains recommended.
 */
#include "lvgl.h"

#if LV_FONT_MONTSERRAT_28 || LV_FONT_MONTSERRAT_48

/* Stringify after macro expansion so MONO_ALIAS_* become real symbol names. */
#define _MONO_ALIAS_STR(x) #x
#define MONO_ALIAS_STR(x) _MONO_ALIAS_STR(x)
#define PROVIDE_FONT_ALIAS(sym, target) \
	__asm__(".global " #sym "\n\t.set " #sym ", " MONO_ALIAS_STR(target))

#if LV_FONT_MONTSERRAT_28
#define MONO_ALIAS_SMALL lv_font_montserrat_28
#else
#define MONO_ALIAS_SMALL lv_font_montserrat_48
#endif

#if LV_FONT_MONTSERRAT_48
#define MONO_ALIAS_LARGE lv_font_montserrat_48
#else
#define MONO_ALIAS_LARGE lv_font_montserrat_28
#endif

#if !LV_FONT_MONTSERRAT_8
PROVIDE_FONT_ALIAS(lv_font_montserrat_8, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_8;
#endif
#if !LV_FONT_MONTSERRAT_10
PROVIDE_FONT_ALIAS(lv_font_montserrat_10, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_10;
#endif
#if !LV_FONT_MONTSERRAT_12
PROVIDE_FONT_ALIAS(lv_font_montserrat_12, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_12;
#endif
#if !LV_FONT_MONTSERRAT_14
PROVIDE_FONT_ALIAS(lv_font_montserrat_14, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_14;
#endif
#if !LV_FONT_MONTSERRAT_16
PROVIDE_FONT_ALIAS(lv_font_montserrat_16, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_16;
#endif
#if !LV_FONT_MONTSERRAT_18
PROVIDE_FONT_ALIAS(lv_font_montserrat_18, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_18;
#endif
#if !LV_FONT_MONTSERRAT_20
PROVIDE_FONT_ALIAS(lv_font_montserrat_20, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_20;
#endif
#if !LV_FONT_MONTSERRAT_22
PROVIDE_FONT_ALIAS(lv_font_montserrat_22, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_22;
#endif
#if !LV_FONT_MONTSERRAT_24
PROVIDE_FONT_ALIAS(lv_font_montserrat_24, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_24;
#endif
#if !LV_FONT_MONTSERRAT_26
PROVIDE_FONT_ALIAS(lv_font_montserrat_26, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_26;
#endif
#if !LV_FONT_MONTSERRAT_30
PROVIDE_FONT_ALIAS(lv_font_montserrat_30, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_30;
#endif
#if !LV_FONT_MONTSERRAT_32
PROVIDE_FONT_ALIAS(lv_font_montserrat_32, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_32;
#endif
#if !LV_FONT_MONTSERRAT_34
PROVIDE_FONT_ALIAS(lv_font_montserrat_34, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_34;
#endif
#if !LV_FONT_MONTSERRAT_36
PROVIDE_FONT_ALIAS(lv_font_montserrat_36, MONO_ALIAS_SMALL);
extern const lv_font_t lv_font_montserrat_36;
#endif
#if !LV_FONT_MONTSERRAT_38
PROVIDE_FONT_ALIAS(lv_font_montserrat_38, MONO_ALIAS_LARGE);
extern const lv_font_t lv_font_montserrat_38;
#endif
#if !LV_FONT_MONTSERRAT_40
PROVIDE_FONT_ALIAS(lv_font_montserrat_40, MONO_ALIAS_LARGE);
extern const lv_font_t lv_font_montserrat_40;
#endif
#if !LV_FONT_MONTSERRAT_42
PROVIDE_FONT_ALIAS(lv_font_montserrat_42, MONO_ALIAS_LARGE);
extern const lv_font_t lv_font_montserrat_42;
#endif
#if !LV_FONT_MONTSERRAT_44
PROVIDE_FONT_ALIAS(lv_font_montserrat_44, MONO_ALIAS_LARGE);
extern const lv_font_t lv_font_montserrat_44;
#endif
#if !LV_FONT_MONTSERRAT_46
PROVIDE_FONT_ALIAS(lv_font_montserrat_46, MONO_ALIAS_LARGE);
extern const lv_font_t lv_font_montserrat_46;
#endif

/*
 * Keep aliases alive under -Wl,--gc-sections (nothing else in ipc_main
 * references the trimmed sizes). -rdynamic then exports them for dlopen.
 */
__attribute__((used)) static const void *const s_montserrat_alias_keep[] = {
#if !LV_FONT_MONTSERRAT_8
	&lv_font_montserrat_8,
#endif
#if !LV_FONT_MONTSERRAT_10
	&lv_font_montserrat_10,
#endif
#if !LV_FONT_MONTSERRAT_12
	&lv_font_montserrat_12,
#endif
#if !LV_FONT_MONTSERRAT_14
	&lv_font_montserrat_14,
#endif
#if !LV_FONT_MONTSERRAT_16
	&lv_font_montserrat_16,
#endif
#if !LV_FONT_MONTSERRAT_18
	&lv_font_montserrat_18,
#endif
#if !LV_FONT_MONTSERRAT_20
	&lv_font_montserrat_20,
#endif
#if !LV_FONT_MONTSERRAT_22
	&lv_font_montserrat_22,
#endif
#if !LV_FONT_MONTSERRAT_24
	&lv_font_montserrat_24,
#endif
#if !LV_FONT_MONTSERRAT_26
	&lv_font_montserrat_26,
#endif
#if !LV_FONT_MONTSERRAT_30
	&lv_font_montserrat_30,
#endif
#if !LV_FONT_MONTSERRAT_32
	&lv_font_montserrat_32,
#endif
#if !LV_FONT_MONTSERRAT_34
	&lv_font_montserrat_34,
#endif
#if !LV_FONT_MONTSERRAT_36
	&lv_font_montserrat_36,
#endif
#if !LV_FONT_MONTSERRAT_38
	&lv_font_montserrat_38,
#endif
#if !LV_FONT_MONTSERRAT_40
	&lv_font_montserrat_40,
#endif
#if !LV_FONT_MONTSERRAT_42
	&lv_font_montserrat_42,
#endif
#if !LV_FONT_MONTSERRAT_44
	&lv_font_montserrat_44,
#endif
#if !LV_FONT_MONTSERRAT_46
	&lv_font_montserrat_46,
#endif
	0,
};

#endif /* LV_FONT_MONTSERRAT_28 || LV_FONT_MONTSERRAT_48 */
