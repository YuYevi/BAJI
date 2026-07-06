#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <font_awesome.h>

#include "display.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"

#define TAG "Display"

Display::Display() {
}

Display::~Display() {
}

void Display::SetStatus(const char* status) {
    
}

void Display::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void Display::ShowNotification(const char* notification, int duration_ms) {
    
}

void Display::ShowPersistentNotification(const char* notification, bool top) {
    (void)top;
    ShowNotification(notification, 0);
}

void Display::UpdateStatusBar(bool update_all) {
}

void Display::ShowChargingFullscreen(bool show) {
    (void)show;
}

void Display::SetEmotion(const char* emotion) {
    
}

void Display::SetChatMessage(const char* role, const char* content) {
    
    
}

void Display::ClearChatMessages() {
    
}

void Display::SetTheme(Theme* theme) {
    current_theme_ = theme;
    Settings settings("display", true);
    settings.SetString("theme", theme->name());
}

void Display::SetPowerSaveMode(bool on) {
    
}
