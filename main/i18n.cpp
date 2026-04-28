#include "i18n.h"
#include "sd_storage.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "i18n";
static Lang s_lang = Lang::EN;

static const char *EN[STR_COUNT] = {
    [STR_SUBTITLE]          = "Scratch on ESP32-P4",
    [STR_FOOTER_HINT]       = "A: Select  B: Back",
    [STR_GAMEPAD_OK]        = " Gamepad",
    [STR_GAMEPAD_NONE]      = " No Gamepad",

    [STR_MAIN_GAMES]        = "Games",
    [STR_MAIN_NEW_GAME]     = "New Game (QR)",
    [STR_MAIN_SETTINGS]     = "Settings",

    [STR_GAMES_TITLE]       = "Saved Games",
    [STR_BACK]              = "Back",
    [STR_GAMES_EMPTY]       = "No saved games.\nUse \"New Game\" to download one.",
    [STR_GAMES_HINT]        = "A: Play  X: Delete  B: Back",

    [STR_SETTINGS_TITLE]    = "Settings",
    [STR_BRIGHTNESS]        = "Brightness",
    [STR_MUTE_AUDIO]        = "Mute Audio",
    [STR_LANGUAGE]          = "Language",
    [STR_NETWORK]           = "Network",
    [STR_WIFI_SAVED_FMT]    = "Saved: %s",
    [STR_WIFI_NONE]         = "No WiFi configured",
    [STR_WIFI_SCAN_QR]      = "Scan WiFi QR",
    [STR_SETTINGS_HINT]     = "Move  Adjust  A: Select  B: Back",

    [STR_YES]               = "Yes",
    [STR_NO]                = "No",
    [STR_CONFIRM]           = "Confirm",
    [STR_DELETE_FMT]        = "Delete \"%s\"?",
    [STR_RETURN_TO_MENU]    = "Return to Menu?",

    [STR_WIFI_CONNECTING]   = "Connecting to WiFi...",
    [STR_WIFI_CONNECTED]    = "Connected!",
    [STR_WIFI_FAILED]       = "WiFi Failed",
    [STR_DOWNLOADING]       = "Downloading...",
    [STR_DL_COMPLETE]       = "Download Complete!",
    [STR_DL_FAILED]         = "Download Failed",
    [STR_LOADING]           = "Loading...",
    [STR_LOAD_FAILED]       = "Load Failed",
    [STR_STARTING]          = "Starting!",
    [STR_NO_WIFI]           = "No WiFi",
    [STR_NO_WIFI_DETAIL]    = "Set up WiFi in Settings first",
    [STR_BUTTON_MAP]        = "Button Map",
    [STR_BUTTON_MAP_DETAIL] = "D-Pad:Arrows A:Space",
    [STR_FETCHING_INFO]     = "Fetching info...",
    [STR_LOADING_JSON]      = "Loading JSON...",
    [STR_ASSETS]            = "Assets",
};

static const char *JP[STR_COUNT] = {
    [STR_SUBTITLE]          = "ESP32-P4 で スクラッチ",
    [STR_FOOTER_HINT]       = "A: 決定  B: 戻る",
    [STR_GAMEPAD_OK]        = " ゲームパッド",
    [STR_GAMEPAD_NONE]      = " 未接続",

    [STR_MAIN_GAMES]        = "ゲーム一覧",
    [STR_MAIN_NEW_GAME]     = "新しいゲーム (QR)",
    [STR_MAIN_SETTINGS]     = "設定",

    [STR_GAMES_TITLE]       = "保存済みゲーム",
    [STR_BACK]              = "戻る",
    [STR_GAMES_EMPTY]       = "保存されたゲームがありません。\n「新しいゲーム」からダウンロードしてください。",
    [STR_GAMES_HINT]        = "A: 開始  X: 削除  B: 戻る",

    [STR_SETTINGS_TITLE]    = "設定",
    [STR_BRIGHTNESS]        = "明るさ",
    [STR_MUTE_AUDIO]        = "ミュート",
    [STR_LANGUAGE]          = "言語",
    [STR_NETWORK]           = "ネットワーク",
    [STR_WIFI_SAVED_FMT]    = "保存済: %s",
    [STR_WIFI_NONE]         = "WiFi 未設定",
    [STR_WIFI_SCAN_QR]      = "WiFi QR をスキャン",
    [STR_SETTINGS_HINT]     = "移動  調整  A: 決定  B: 戻る",

    [STR_YES]               = "はい",
    [STR_NO]                = "いいえ",
    [STR_CONFIRM]           = "確認",
    [STR_DELETE_FMT]        = "「%s」を削除しますか？",
    [STR_RETURN_TO_MENU]    = "メニューに戻りますか？",

    [STR_WIFI_CONNECTING]   = "WiFi に接続中...",
    [STR_WIFI_CONNECTED]    = "接続しました！",
    [STR_WIFI_FAILED]       = "WiFi 接続失敗",
    [STR_DOWNLOADING]       = "ダウンロード中...",
    [STR_DL_COMPLETE]       = "ダウンロード完了！",
    [STR_DL_FAILED]         = "ダウンロード失敗",
    [STR_LOADING]           = "読み込み中...",
    [STR_LOAD_FAILED]       = "読み込み失敗",
    [STR_STARTING]          = "開始！",
    [STR_NO_WIFI]           = "WiFi 未接続",
    [STR_NO_WIFI_DETAIL]    = "設定から WiFi をセットアップしてください",
    [STR_BUTTON_MAP]        = "ボタン配置",
    [STR_BUTTON_MAP_DETAIL] = "十字キー:矢印 A:スペース",
    [STR_FETCHING_INFO]     = "情報を取得中...",
    [STR_LOADING_JSON]      = "JSON 読込中...",
    [STR_ASSETS]            = "アセット",
};

void lang_init()
{
    char buf[8];
    if (sd_load_lang(buf, sizeof(buf))) {
        if (strcmp(buf, "jp") == 0) s_lang = Lang::JP;
        else                        s_lang = Lang::EN;
        ESP_LOGI(TAG, "Loaded language: %s", buf);
    }
}

Lang lang_get() { return s_lang; }

void lang_set(Lang l)
{
    s_lang = l;
    sd_save_lang(l == Lang::JP ? "jp" : "en");
}

const char *tr(StringId id)
{
    if (id < 0 || id >= STR_COUNT) return "";
    const char *s = (s_lang == Lang::JP ? JP : EN)[id];
    return s ? s : "";
}
