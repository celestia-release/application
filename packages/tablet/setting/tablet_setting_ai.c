#define LOG_TAG "TABLET_SETTING_AI"

#include "log.h"
#include "multimedia.h"
#include "app_main.h"
#include "tablet_setting_ai.h"
#include "tablet_setting.h"
#include "tablet_font.h"

#include <string.h>
#include <stdio.h>

static int setting_ai_manager_get_config(media_ai_config_t *out, int *mode_count)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();
    if (!mm || !mm->camera || !mm->camera->ai_get_config) {
        return 0;
    }
    return mm->camera->ai_get_config(out, mode_count);
}

static int setting_ai_manager_set_config(media_ai_config_t *config)
{
    multimedia_t *mm = (multimedia_t *)app_main_get_multimedia();
    if (!mm || !mm->camera || !mm->camera->ai_set_config) {
        return -1;
    }
    return mm->camera->ai_set_config(config);
}

/* List box with checkboxes */
typedef struct {
    lv_obj_t *list_box;           /* List container */
    lv_obj_t **checkboxes, **lables;  /* Checkbox and label arrays */
    lv_obj_t *select_all_btn;     /* Select-all button */
    media_ai_config_t *options;   /* Option entries */
    uint16_t option_count;       /* Number of options */
    bool *selected;              /* Selected state per option */
    bool select_all_state;       /* Select-all state */
} list_box_with_checks_t;

static void checkbox_event_handler(lv_event_t *e);
static void confirm_event_handler(lv_event_t *e);
static void select_all_event_handler(lv_event_t *e);
#define MAX_AI_CONFIG_COUNT 16

static media_ai_config_t g_ai_config[MAX_AI_CONFIG_COUNT] = {0};
static int g_ai_config_count = 0;

int enable_or_disable_mode_init(list_box_with_checks_t *lb);
void update_selected_display(list_box_with_checks_t *lb);
uint16_t dis_selected_index = 0;

static void dis_dropdown_event_handler(lv_event_t * e) {

    lv_obj_t * dropdown = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    if (list) {
        lv_font_t *f = tablet_font_get_freetype_font(20);
        if (f)
            lv_obj_set_style_text_font(list, f, 0);
    }

    if (code == LV_EVENT_VALUE_CHANGED) {
        char selected_text[32];
        lv_dropdown_get_selected_str(dropdown, selected_text, sizeof(selected_text));
        dis_selected_index = lv_dropdown_get_selected(dropdown);

        log_debug("dropdown selected: %s, index=%d", selected_text, dis_selected_index);

        LV_LOG_USER("Option dis_selected: %s (index: %d)", selected_text, dis_selected_index);
    }
}

/* Create list box with checkboxes */
list_box_with_checks_t *create_list_box_with_checks(lv_obj_t *parent, const char *title, media_ai_config_t *options, uint16_t count)
{
    /* Allocate */
    list_box_with_checks_t *lb = lv_malloc(sizeof(list_box_with_checks_t));
    if (!lb) return NULL;
    lb->options = options;
    lb->option_count = count;
    lb->selected = lv_malloc(sizeof(bool) * count);
    if (!lb->selected) { lv_free(lb); return NULL; }
    memset(lb->selected, 0, sizeof(bool) * count);
    for (int i = 0; i < lb->option_count; i++) {
        lb->selected[i] = (bool)options[i].enable;
    }

    lb->select_all_state = false;
    lb->checkboxes = lv_malloc(sizeof(lv_obj_t*) * count);
    lb->lables = lv_malloc(sizeof(lv_obj_t*) * count);
    if (!lb->checkboxes || !lb->lables) {
        lv_free(lb->selected);
        lv_free(lb);
        return NULL;
    }

    /* Title label (optional) */
    // lv_obj_t *title_label = lv_label_create(parent);
    // lv_label_set_text(title_label, title);
    // lv_obj_set_style_text_font(title_label, (const lv_font_t*)tablet_font_get_freetype_font(20), 0);
    // lv_obj_set_style_text_color(title_label, lv_color_hex(0x3498db), 0);

    lv_obj_t* ai_parent = lv_obj_create(parent);
    lv_obj_set_size(ai_parent, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(ai_parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(ai_parent, 10, 0);
    lv_obj_set_style_bg_color(ai_parent, lv_color_hex(SETTING_CONTENT_BG), 0);
    lv_obj_set_style_border_width(ai_parent, 0, 0);
    lv_obj_set_style_radius(ai_parent, 10, 0);
    lv_obj_remove_flag(ai_parent, LV_OBJ_FLAG_SCROLLABLE);

    /* List container: flex-grow so detection rows use most horizontal space */
    lb->list_box = lv_obj_create(ai_parent);
    lv_obj_set_width(lb->list_box, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(lb->list_box, 1);
    lv_obj_set_height(lb->list_box, LV_PCT(100));
    lv_obj_align(lb->list_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_flex_flow(lb->list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(lb->list_box, 10, 0);
    lv_obj_set_style_bg_color(lb->list_box, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_border_width(lb->list_box, 1, 0);
    lv_obj_set_style_border_color(lb->list_box, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(lb->list_box, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_radius(lb->list_box, GUI_SCALE(16), 0);
    lv_obj_set_style_shadow_width(lb->list_box, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_ofs_y(lb->list_box, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_shadow_color(lb->list_box, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(lb->list_box, LV_OPA_40, 0);

    /* Select-all button (optional) */
    // lb->select_all_btn = lv_btn_create(lb->list_box);
    // lv_obj_set_size(lb->select_all_btn, LV_PCT(100), 40);
    // lv_obj_set_style_bg_color(lb->select_all_btn, lv_color_hex(0x3498db), 0);
    // lv_obj_set_style_radius(lb->select_all_btn, 5, 0);

    // lv_obj_t *btn_label = lv_label_create(lb->select_all_btn);
    // lv_label_set_text(btn_label, lan("select all"));
    // lv_obj_center(btn_label);


    /* One list box per model; add options */
    media_ai_mode_config_t* cur_mode_info = NULL;
    /* Section title for first row so user knows what the options are */
    lv_obj_t *section_title = lv_label_create(lb->list_box);
    lv_label_set_text(section_title, lan("AI options") ? lan("AI options") : "AI options");
    lv_obj_set_style_text_color(section_title, lv_color_hex(SETTING_TEXT_SEC), 0);
    lv_obj_set_style_text_font(section_title, tablet_font_get_freetype_font(18) ? tablet_font_get_freetype_font(18) : tablet_font_get_montserrat_font(14), 0);
    /* First sub_list_box created when we add the first item (so first row is not empty) */
    lv_obj_t *sub_list_box = NULL;

    dis_selected_index = 0;
    for (int i = 0; i < count; i++) {
        log_info("options[%d] name=%s enable=%d distance_flag=%d mode_info=%p",
                 i, options[i].name, options[i].enable, options[i].distance_flag, (void *)options[i].mode_info);
        if (sub_list_box == NULL || (options[i].mode_info != cur_mode_info && options[i].mode_info != NULL)) {
            if (sub_list_box != NULL) {
                /* Separator line before next group */
                lv_obj_t *separator = lv_line_create(lb->list_box);
                static lv_point_precise_t points[] = {{0, 0}, {280, 0}};
                lv_line_set_points(separator, points, 2);
                lv_obj_set_style_line_width(separator, 2, 0);
                lv_obj_set_style_line_color(separator, lv_color_hex(SETTING_BORDER), 0);
            }
            sub_list_box = lv_obj_create(lb->list_box);
            lv_obj_set_width(sub_list_box, LV_PCT(100));
            lv_obj_set_height(sub_list_box, LV_SIZE_CONTENT);
            lv_obj_set_style_min_height(sub_list_box, 44, 0);
            lv_obj_set_flex_flow(sub_list_box, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_all(sub_list_box, 6, 0);
            lv_obj_set_style_pad_column(sub_list_box, 4, 0);
            lv_obj_set_style_bg_color(sub_list_box, lv_color_hex(SETTING_CARD_ITEM), 0);
            lv_obj_set_style_border_width(sub_list_box, 1, 0);
            lv_obj_set_style_border_color(sub_list_box, lv_color_hex(SETTING_BORDER), 0);
            lv_obj_set_style_radius(sub_list_box, GUI_SCALE(10), 0);
            if (options[i].mode_info != NULL)
                cur_mode_info = options[i].mode_info;
        }

        lv_obj_t *item = lv_obj_create(sub_list_box);
        lv_obj_set_flex_grow(item, 1);
        lv_obj_set_style_min_width(item, 0, 0);
        lv_obj_set_height(item, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(item, 40, 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_0, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(item, 2, 0);
        lv_obj_set_style_pad_column(item, 4, 0);

        /* Checkbox (style for visibility on dark background) */
        lb->checkboxes[i] = lv_checkbox_create(item);
        lv_checkbox_set_text(lb->checkboxes[i], "");
        lv_obj_set_user_data(lb->checkboxes[i], (void *)(intptr_t)i);
        lv_obj_set_state(lb->checkboxes[i], LV_STATE_CHECKED, options[i].enable == 1 ? LV_STATE_CHECKED : 0);
        lv_obj_set_style_text_color(lb->checkboxes[i], lv_color_hex(SETTING_TEXT), 0);
        lv_obj_set_style_bg_color(lb->checkboxes[i], lv_color_hex(SETTING_BORDER), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(lb->checkboxes[i], lv_color_hex(SETTING_ACCENT_OK), LV_PART_INDICATOR | LV_STATE_CHECKED);

        /* Option label */
        lv_obj_t *label = lv_label_create(item);
        lb->lables[i] = label;
        lv_label_set_text(label, lan(options[i].name));
        lv_font_t *font_item = tablet_font_get_freetype_font(18);
        if (!font_item)
            font_item = tablet_font_get_freetype_font(20);
        if (font_item)
            lv_obj_set_style_text_font(label, font_item, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(SETTING_TEXT), 0);
        lv_obj_set_style_min_width(label, 0, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_flex_grow(label, 1);

        if (options[i].distance_flag)
        {
            dis_selected_index = 1;
        }
        /* Add event */
        lv_obj_add_event_cb(lb->checkboxes[i], checkbox_event_handler, LV_EVENT_VALUE_CHANGED, lb);
    }

    lv_obj_t * right_side_obj = lv_obj_create(ai_parent);
    lv_obj_set_width(right_side_obj, 108);
    lv_obj_set_height(right_side_obj, LV_PCT(100));
    lv_obj_align(right_side_obj, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(right_side_obj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(right_side_obj, 10, 0);
    lv_obj_set_style_bg_color(right_side_obj, lv_color_hex(SETTING_CARD_BG), 0);
    lv_obj_set_style_border_width(right_side_obj, 1, 0);
    lv_obj_set_style_border_color(right_side_obj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(right_side_obj, SETTING_CARD_BORDER_OPA, 0);
    lv_obj_set_style_radius(right_side_obj, GUI_SCALE(16), 0);
    lv_obj_set_style_shadow_width(right_side_obj, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_ofs_y(right_side_obj, GUI_SCALE_VER(3), 0);
    lv_obj_set_style_shadow_color(right_side_obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(right_side_obj, LV_OPA_40, 0);

    lv_obj_t * dropdown = lv_dropdown_create(right_side_obj);
    lv_font_t *font_20 = tablet_font_get_freetype_font(20);
    if (font_20)
        lv_obj_set_style_text_font(dropdown, font_20, 0);
    lv_obj_align(dropdown, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_size(dropdown, 100, 50);
    lv_dropdown_set_options(dropdown, "正常距离\n远距离");
    lv_dropdown_set_selected(dropdown, dis_selected_index);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    if (list && font_20)
        lv_obj_set_style_text_font(list, font_20, 0);
    lv_obj_add_event_cb(dropdown, dis_dropdown_event_handler, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t * spacer = lv_obj_create(right_side_obj);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);

    /* Confirm button */
    lv_obj_t *confirm_btn = lv_btn_create(right_side_obj);
    // lv_obj_set_size(confirm_btn, LV_PCT(100), 50);
    /* Align window to parent right-center */
    lv_obj_align(confirm_btn, LV_ALIGN_BOTTOM_RIGHT, 5, 0);
    lv_obj_set_size(confirm_btn, 100, 50);

    /* Confirm button: same blue gradient as the launcher's generated icons. */
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(SETTING_ACCENT), 0);
    lv_obj_set_style_bg_grad_color(confirm_btn, lv_color_hex(SETTING_ACCENT_DEEP), 0);
    lv_obj_set_style_bg_grad_dir(confirm_btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(confirm_btn, GUI_SCALE(12), 0);
    lv_obj_set_style_shadow_width(confirm_btn, GUI_SCALE(8), 0);
    lv_obj_set_style_shadow_ofs_y(confirm_btn, GUI_SCALE_VER(2), 0);
    lv_obj_set_style_shadow_color(confirm_btn, lv_color_hex(SETTING_ACCENT_DEEP), 0);
    lv_obj_set_style_shadow_opa(confirm_btn, LV_OPA_50, 0);

    lv_obj_t *confirm_label = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_label, lan("ok"));
    lv_obj_center(confirm_label);
    lv_obj_set_style_text_color(confirm_label, lv_color_white(), 0);
    lv_obj_add_event_cb(confirm_btn, confirm_event_handler, LV_EVENT_CLICKED, lb);

    enable_or_disable_mode_init(lb);

    /* Select-all button event (optional) */
    //lv_obj_add_event_cb(lb->select_all_btn, select_all_event_handler, LV_EVENT_CLICKED, lb);

    return lb;
}

/**
 * Check if selected mode count exceeds limit or conflicts.
 * @return 0 ok, 1 max modes reached, 2 conflict with existing selection.
 */
 //判断模型是否超过最大同时运行个数
 //返回值: 0表示检查通过; 1表示达到最大运行个数; 2表示当前选择的模型与之前的模型有冲突
 int check_mode(list_box_with_checks_t *lb, int select_id)
 {
    media_ai_mode_config_t* cur_mode_info = NULL;
    int selected_mode_count = 0;
    int gesture_flag = 0;

    if (2 == select_id)
    {
        gesture_flag = 1;
    }
    for (int i = 0; i < g_ai_config_count; i++)
    {
        //log_warn("g_ai_config[%d, enable: %d], mode_info:%p[enable: %d, name: %s], cur_mode_info:%p, select_id: %d", i, g_ai_config[i].enable, g_ai_config[i].mode_info, g_ai_config[i].mode_info->enable, g_ai_config[i].mode_info->name, cur_mode_info, select_id);
        if(g_ai_config[i].enable == 0)
            continue;

        log_warn("g_ai_config[%d].mode_info:%p, cur_mode_info:%p", i, g_ai_config[i].mode_info, cur_mode_info);
        if (2 == i)
        {
            gesture_flag = 1;
        }
        if(g_ai_config[i].mode_info != cur_mode_info)
        {
            selected_mode_count++;
            cur_mode_info = g_ai_config[i].mode_info;
        }

        if(g_ai_config[i].mode_info == g_ai_config[select_id].mode_info)
        {
            //old_mode
            return 0;
        }
    }

    log_warn("selected_mode_count=%d", selected_mode_count);
 #if 0
     if (selected_mode_count >= MEDIA_AI_MAX_RUN_MODE_COUNT)
     {
         return 1;
     }
 #else
     if (gesture_flag && selected_mode_count >= MEDIA_AI_MAX_RUN_MODE_COUNT)
     {
         return 1;
     }
 #endif
     return 0;
 }

int enable_or_disable_obj(list_box_with_checks_t *lb, int index, int enable)
{
    bool is_disable = lv_obj_has_state(lb->checkboxes[index], LV_STATE_DISABLED);
    //log_warn("enable_or_disable_obj index: %d, enable: %d, is_disable: %d",
    //index, enable, is_disable);
    if ((!is_disable && enable) || (is_disable && !enable))
    {
        return 0;
    }
    if(enable)
    {
        lv_obj_clear_state(lb->checkboxes[index], LV_STATE_DISABLED);
        lv_obj_set_style_text_color(lb->lables[index], lv_color_white(), 0);
    }
    else
    {
        lv_obj_add_state(lb->checkboxes[index], LV_STATE_DISABLED);
        //lv_obj_set_style_text_color(lb->lables[index], lv_color_white(), 0);
        // 使用调色板灰度 (0-9, 0最暗, 9最亮)
        lv_obj_set_style_text_color(lb->lables[index], lv_color_hex(0x808080), 0);
    }
    return 1;
}

int enable_or_disable_mode(list_box_with_checks_t *lb, int index, int enable)
{
    //log_warn("enable disable checkbox index: %d, enable: %d, g_ai_config[%d, %d, %d, %d, %d, %d, %d]",
    //index, enable, g_ai_config[0].enable, g_ai_config[1].enable, g_ai_config[2].enable,
    //g_ai_config[3].enable, g_ai_config[4].enable, g_ai_config[5].enable, g_ai_config[6].enable);
    if(enable)
    {
        if(index == 2)
        {
            // 禁用复选框（不可交互）
            enable_or_disable_obj(lb, 0, 0);
            enable_or_disable_obj(lb, 1, 0);
            enable_or_disable_obj(lb, 3, 0);
            enable_or_disable_obj(lb, 4, 0);
            enable_or_disable_obj(lb, 5, 0);
            enable_or_disable_obj(lb, 6, 0);
            //log_warn("index=%d enable=%d", index, enable);
        }
        else if(index == 0 || index == 1)
        {
            // 禁用复选框（不可交互）
            enable_or_disable_obj(lb, 2, 0);
            //log_warn("index=%d enable=%d", index, enable);
        }
        else if(index == 3)
        {
            // 禁用复选框（不可交互）
            enable_or_disable_obj(lb, 2, 0);
            enable_or_disable_obj(lb, 4, 0);
            enable_or_disable_obj(lb, 5, 0);
            enable_or_disable_obj(lb, 6, 0);
            //log_warn("index=%d enable=%d", index, enable);
        }
        else if(index == 4 || index == 5 || index == 6)
        {
            // 禁用复选框（不可交互）
            enable_or_disable_obj(lb, 2, 0);
            enable_or_disable_obj(lb, 3, 0);
            //log_warn("index=%d enable=%d", index, enable);
        }
    }
    else
    {
        if(index == 2)
        {
            // 禁用复选框（不可交互）
            enable_or_disable_obj(lb, 0, 1);
            enable_or_disable_obj(lb, 1, 1);
            enable_or_disable_obj(lb, 3, 1);
            enable_or_disable_obj(lb, 4, 1);
            enable_or_disable_obj(lb, 5, 1);
            enable_or_disable_obj(lb, 6, 1);
            //log_warn("index=%d enable=%d", index, enable);
        }
        else if(index == 0 || index == 1 || index == 3 || index == 4 || index == 5 || index == 6)
        {
            if(g_ai_config[0].enable == 0 && g_ai_config[1].enable == 0 && g_ai_config[2].enable == 0 && g_ai_config[3].enable == 0 && g_ai_config[4].enable == 0 && g_ai_config[5].enable == 0 && g_ai_config[6].enable == 0)
            {
                enable_or_disable_obj(lb, 0, 1);
                enable_or_disable_obj(lb, 1, 1);
                enable_or_disable_obj(lb, 2, 1);
                enable_or_disable_obj(lb, 3, 1);
                enable_or_disable_obj(lb, 4, 1);
                enable_or_disable_obj(lb, 5, 1);
                enable_or_disable_obj(lb, 6, 1);
            }
            else if(g_ai_config[0].enable == 0 && g_ai_config[1].enable == 0 && g_ai_config[3].enable == 0 && g_ai_config[4].enable == 0 && g_ai_config[5].enable == 0 && g_ai_config[6].enable == 0)
            // 禁用复选框（不可交互）
            {
                enable_or_disable_obj(lb, 2, 1);
                //log_warn("index=%d enable=%d", index, enable);
            }
            else if (g_ai_config[3].enable == 0 && g_ai_config[4].enable == 0 && g_ai_config[5].enable == 0 && g_ai_config[6].enable == 0)
            {
                enable_or_disable_obj(lb, 3, 1);
                enable_or_disable_obj(lb, 4, 1);
                enable_or_disable_obj(lb, 5, 1);
                enable_or_disable_obj(lb, 6, 1);
            }
            else if (g_ai_config[3].enable == 0)
            {
                enable_or_disable_obj(lb, 4, 1);
                enable_or_disable_obj(lb, 5, 1);
                enable_or_disable_obj(lb, 6, 1);
            }
            else if (g_ai_config[4].enable == 0 && g_ai_config[5].enable == 0 && g_ai_config[6].enable == 0)
            {
                enable_or_disable_obj(lb, 3, 1);
            }
        }
    }

    //log_warn("index=%d enable=%d", index, enable);
}

int enable_or_disable_mode_init(list_box_with_checks_t *lb)
{
    if(g_ai_config[2].enable)
    {
        enable_or_disable_obj(lb, 0, 0);
        enable_or_disable_obj(lb, 1, 0);
        enable_or_disable_obj(lb, 3, 0);
        enable_or_disable_obj(lb, 4, 0);
        enable_or_disable_obj(lb, 5, 0);
        enable_or_disable_obj(lb, 6, 0);
    }
    else if(g_ai_config[3].enable)
    {
        enable_or_disable_obj(lb, 2, 0);
        enable_or_disable_obj(lb, 4, 0);
        enable_or_disable_obj(lb, 5, 0);
        enable_or_disable_obj(lb, 6, 0);
    }
    else if(g_ai_config[4].enable || g_ai_config[5].enable || g_ai_config[6].enable)
    {
        enable_or_disable_obj(lb, 2, 0);
        enable_or_disable_obj(lb, 3, 0);
    }
    else if(g_ai_config[0].enable || g_ai_config[1].enable)
    {
        enable_or_disable_obj(lb, 2, 0);
    }

    //log_warn("index=%d enable=%d", index, enable);
}

/* Checkbox event handler */
static void checkbox_event_handler(lv_event_t *e) {
    lv_obj_t *cb = lv_event_get_target(e);
    list_box_with_checks_t *lb = lv_event_get_user_data(e);

    uint16_t index = (uint16_t)(intptr_t)lv_obj_get_user_data(cb);
    bool checked = lv_obj_get_state(cb) & LV_STATE_CHECKED;
#if 0
    int ret = check_mode(lb, index);
    if(ret != 0)
    {
        if(ret == 1)
        {
            char msg[64] = {0};
            sprintf(msg,"%s(%d)", lan("mode already reach max"), MEDIA_AI_MAX_RUN_MODE_COUNT);
            show_msgbox_note(msg);
        }
        // else if(ret == 2)
        // {
        //     show_msgbox_note(lan("mode Conflict"));
        // }

        lv_obj_set_state(lb->checkboxes[index], LV_STATE_CHECKED, 0);

        return;
    }
#endif
    lb->selected[index] = checked;
    g_ai_config[index].enable = checked;

    enable_or_disable_mode(lb, index, checked);
}

/* Select-all button handler (optional) */
/*static void select_all_event_handler(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    list_box_with_checks_t *lb = lv_event_get_user_data(e);

    lb->select_all_state = !lb->select_all_state;

    for (int i = 0; i < lb->option_count; i++) {
        lb->selected[i] = lb->select_all_state;
        lv_obj_set_state(lb->checkboxes[i], LV_STATE_CHECKED, lb->select_all_state ? LV_STATE_CHECKED : 0);//??
    }

    lv_label_set_text(lv_obj_get_child(btn, 0),
                      lb->select_all_state ? lan("cancel select all") : lan("select all"));

}*/

/* Confirm button handler: save selection and apply config */
static void confirm_event_handler(lv_event_t *e) {
    list_box_with_checks_t *lb = lv_event_get_user_data(e);
    for (int i = 0; i < lb->option_count; i++) {
        if (lb->selected[i]) {
            LV_LOG_USER("selected: %s", lb->options[i].name);
            g_ai_config[i].enable = 1;
            g_ai_config[i].distance_flag = dis_selected_index;
        }
        else
        {
            g_ai_config[i].enable = 0;
            g_ai_config[i].distance_flag = 0;
        }
        //log_warn("confirm_event_handler %d/%d: enable[%d], distance_flag[%d]", i, lb->option_count, g_ai_config[i].enable, g_ai_config[i].distance_flag);
    }

    (void)setting_ai_manager_set_config(g_ai_config);

    show_msgbox_note(lan("config ok"));
}

typedef struct
{
    int type;
    int enable;
    char name[24];
}ai_obj_type;

lv_result_t tablet_setting_ai_create(lv_obj_t *parent)
{
    log_info("[ai] create enter, parent=%p", (void *)parent);
    int mode_count = 0;
    g_ai_config_count = setting_ai_manager_get_config(&g_ai_config[0], &mode_count);
    log_info("[ai] config: count=%d mode_count=%d", g_ai_config_count, mode_count);

    /* When no AI config (count=0), show placeholder and avoid create_list_box_with_checks
     * which can crash on font/layout path when options array is empty. */
    if (g_ai_config_count <= 0) {
        log_info("[ai] count=0, show placeholder");
        lv_obj_t *cont = lv_obj_create(parent);
        lv_obj_set_size(cont, LV_PCT(90), LV_PCT(60));
        lv_obj_center(cont);
        lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(cont, 16, 0);
        lv_obj_set_style_bg_color(cont, lv_color_hex(SETTING_CARD_BG), 0);
        lv_obj_set_style_border_width(cont, 0, 0);
        lv_obj_set_style_radius(cont, 12, 0);
        lv_obj_t *title = lv_label_create(cont);
        lv_label_set_text(title, lan("AI") ? lan("AI") : "AI");
        lv_obj_set_style_text_font(title, tablet_font_get_freetype_font(22), 0);
        lv_obj_set_style_text_color(title, lv_color_hex(SETTING_TEXT), 0);
        lv_obj_t *placeholder = lv_label_create(cont);
        const char *msg = lan("No AI config");
        if (!msg) msg = "No AI config";
        lv_label_set_text(placeholder, msg);
        lv_obj_set_style_text_font(placeholder, tablet_font_get_freetype_font(18), 0);
        lv_obj_set_style_text_color(placeholder, lv_color_hex(SETTING_TEXT_SEC), 0);
        lv_label_set_long_mode(placeholder, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(placeholder, lv_pct(100));
        log_info("[ai] create done (placeholder)");
        return LV_RESULT_OK;
    }

    /* Create list box with checkboxes */
    log_info("[ai] create_list_box, parent=%p count=%d", (void *)parent, g_ai_config_count);
    list_box_with_checks_t *list_box = create_list_box_with_checks(
        parent,
        lan("select object"),
        g_ai_config,
        g_ai_config_count
    );
    log_info("[ai] list_box=%p", (void *)list_box);
    if (!list_box || !list_box->list_box) {
        log_error("[ai] list_box null");
        return LV_RESULT_INVALID;
    }
    log_info("[ai] center list_box");
    lv_obj_center(list_box->list_box);
    log_info("[ai] create done");
    return LV_RESULT_OK;
}
