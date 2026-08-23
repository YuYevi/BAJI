#include "ui.h"
#include "ui_helpers.h"
#include "app_common.h"
#include "ui_media_assets.h"

/* 资源声明 */

/* LVGL 配置检查 */
#if LV_COLOR_DEPTH != 16
#error "LV_COLOR_DEPTH should be 16bit to match SquareLine Studio's settings"
#endif

/* UI 生命周期函数 */

void ui_init(void)
{
    lv_disp_t *dispp = lv_display_get_default();
    lv_theme_t *theme = lv_theme_default_init(
        dispp,
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        false,
        LV_FONT_DEFAULT);

    /* 初始化默认主题并绑定到当前显示设备 */
    lv_disp_set_theme(dispp, theme);

    /* 初始化状态叠加层和各个界面资源 */
    app_status_overlay_init();
    ui_AlarmRuntime_init();
    ui_HomeScreen_init();
    ui_StandbyScreen_init();
    ui_AIChatScreen_init();
    ui_WallpaperScreen_init();
    ui_EventsScreen_init();
    ui_AlarmScreen_init();
    ui_LightStickScreen_init();

    /* 重置导航并注册所有可切换页面 */
    ui_nav_reset();
    ui_nav_register(&ui_StandbyScreen, ui_StandbyScreen_init, ui_StandbyScreen_deinit);
    ui_nav_register(&ui_HomeScreen, ui_HomeScreen_init, ui_HomeScreen_deinit);
    ui_nav_register(&ui_AIChatScreen, ui_AIChatScreen_init, ui_AIChatScreen_deinit);
    ui_nav_register(&ui_WallpaperScreen, ui_WallpaperScreen_init, ui_WallpaperScreen_deinit);
    ui_nav_register(&ui_EventsScreen, ui_EventsScreen_init, ui_EventsScreen_deinit);
    ui_nav_register(&ui_AlarmScreen, ui_AlarmScreen_init, ui_AlarmScreen_deinit);
    ui_nav_register(&ui_LightStickScreen, ui_LightStickScreen_init, ui_LightStickScreen_deinit);

    /* 默认进入待机界面 */
    lv_screen_load(ui_StandbyScreen);
}

void ui_destroy(void)
{
    /* 释放各界面资源 */
    ui_HomeScreen_deinit();
    ui_StandbyScreen_deinit();
    ui_AIChatScreen_deinit();
    ui_WallpaperScreen_deinit();
    ui_EventsScreen_deinit();
    ui_AlarmScreen_deinit();
    ui_AlarmRuntime_deinit();
    ui_LightStickScreen_deinit();

    /* 释放壁纸资源并注销状态叠加层 */
    ui_media_assets_release_all_wallpaper_jpgs();
    app_status_overlay_deinit();
}
