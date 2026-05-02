#include "i18n.h"
#include "sd_storage.h"
#include "esp_log.h"
#include <cstring>

static const char *TAG = "i18n";
static Lang s_lang = Lang::EN;

static const char *EN[STR_COUNT] = {
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
    [STR_VOLUME]            = "Volume",
    [STR_LEVEL_LOW]         = "small",
    [STR_LEVEL_HIGH]        = "big",
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
    [STR_BUTTON_MAP_HINT]   = "B: Close",
    [STR_BUTTON_MAP_NONE]   = "This game doesn't use any buttons.",
    [STR_FETCHING_INFO]     = "Fetching info...",
    [STR_LOADING_JSON]      = "Loading JSON...",
    [STR_ASSETS]            = "Assets",
    [STR_LOADING_SOUNDS]    = "Loading sounds...",
    [STR_LOADING_COSTUMES]  = "Loading costumes...",

    [STR_QR_SCAN_WIFI_HEAD]    = "Show me your Wi-Fi QR!",
    [STR_QR_SCAN_PROJECT_HEAD] = "Show me a Game QR!",
    [STR_QR_HINT_BACK]         = "B: Back",
    [STR_WIFI_TRY_AGAIN]       = "Let's try again",
};

static const char *JP[STR_COUNT] = {
    [STR_FOOTER_HINT]       = "A: きめる  B: もどる",
    [STR_GAMEPAD_OK]        = " コントローラー",
    [STR_GAMEPAD_NONE]      = " コントローラーなし",

    [STR_MAIN_GAMES]        = "あそぶ",
    [STR_MAIN_NEW_GAME]     = "あたらしいゲーム",
    [STR_MAIN_SETTINGS]     = "せってい",

    [STR_GAMES_TITLE]       = "あそべるゲーム",
    [STR_BACK]              = "もどる",
    [STR_GAMES_EMPTY]       = "ゲームがまだないよ。\n「あたらしいゲーム」からダウンロードしてね。",
    [STR_GAMES_HINT]        = "A: あそぶ  X: けす  B: もどる",

    [STR_SETTINGS_TITLE]    = "せってい",
    [STR_BRIGHTNESS]        = "明るさ",
    [STR_MUTE_AUDIO]        = "ミュート",
    [STR_VOLUME]            = "音の大きさ",
    [STR_LEVEL_LOW]         = "ちいさい",
    [STR_LEVEL_HIGH]        = "おおきい",
    [STR_LANGUAGE]          = "ことば",
    [STR_NETWORK]           = "Wi-Fi",
    [STR_WIFI_SAVED_FMT]    = "%s に つないでいるよ",
    [STR_WIFI_NONE]         = "Wi-Fi はまだだよ",
    [STR_WIFI_SCAN_QR]      = "QRを読みとる",
    [STR_SETTINGS_HINT]     = "上下で うごかす  左右で あわせる  A: きめる  B: もどる",

    [STR_YES]               = "はい",
    [STR_NO]                = "いいえ",
    [STR_CONFIRM]           = "かくにん",
    [STR_DELETE_FMT]        = "「%s」を けしていい？",
    [STR_RETURN_TO_MENU]    = "メニューに もどる？",

    [STR_WIFI_CONNECTING]   = "Wi-Fi に つないでいるよ...",
    [STR_WIFI_CONNECTED]    = "つながったよ！",
    [STR_WIFI_FAILED]       = "つながらなかった",
    [STR_DOWNLOADING]       = "ダウンロード中...",
    [STR_DL_COMPLETE]       = "ダウンロードできたよ！",
    [STR_DL_FAILED]         = "ダウンロードできなかった",
    [STR_LOADING]           = "よみこみ中...",
    [STR_LOAD_FAILED]       = "よみこめなかった",
    [STR_STARTING]          = "はじめるよ！",
    [STR_NO_WIFI]           = "Wi-Fi がつながってないよ",
    [STR_NO_WIFI_DETAIL]    = "せっていから Wi-Fi を つないでね",
    [STR_BUTTON_MAP]        = "ボタンの つかいかた",
    [STR_BUTTON_MAP_DETAIL] = "十字キー: やじるし  A: スペース",
    [STR_BUTTON_MAP_HINT]   = "B: とじる",
    [STR_BUTTON_MAP_NONE]   = "このゲームは ボタンを つかわないよ",
    [STR_FETCHING_INFO]     = "じょうほうを とりに行ってるよ...",
    [STR_LOADING_JSON]      = "データを よみこみ中...",
    [STR_ASSETS]            = "そざい",
    [STR_LOADING_SOUNDS]    = "おとを よみこみ中...",
    [STR_LOADING_COSTUMES]  = "えを よみこみ中...",

    [STR_QR_SCAN_WIFI_HEAD]    = "Wi-Fi の QR をかざしてね",
    [STR_QR_SCAN_PROJECT_HEAD] = "ゲームの QR をかざしてね",
    [STR_QR_HINT_BACK]         = "B: もどる",
    [STR_WIFI_TRY_AGAIN]       = "もう一回 やってみよう",
};

static const char *KR[STR_COUNT] = {
    [STR_FOOTER_HINT]       = "A: 선택  B: 뒤로",
    [STR_GAMEPAD_OK]        = " 게임패드",
    [STR_GAMEPAD_NONE]      = " 미연결",

    [STR_MAIN_GAMES]        = "게임 목록",
    [STR_MAIN_NEW_GAME]     = "새 게임 (QR)",
    [STR_MAIN_SETTINGS]     = "설정",

    [STR_GAMES_TITLE]       = "저장된 게임",
    [STR_BACK]              = "뒤로",
    [STR_GAMES_EMPTY]       = "저장된 게임이 없습니다.\n새 게임에서 다운로드하세요.",
    [STR_GAMES_HINT]        = "A: 시작  X: 삭제  B: 뒤로",

    [STR_SETTINGS_TITLE]    = "설정",
    [STR_BRIGHTNESS]        = "밝기",
    [STR_MUTE_AUDIO]        = "음소거",
    [STR_VOLUME]            = "소리 크기",
    [STR_LEVEL_LOW]         = "작게",
    [STR_LEVEL_HIGH]        = "크게",
    [STR_LANGUAGE]          = "언어",
    [STR_NETWORK]           = "네트워크",
    [STR_WIFI_SAVED_FMT]    = "저장됨: %s",
    [STR_WIFI_NONE]         = "WiFi 미설정",
    [STR_WIFI_SCAN_QR]      = "WiFi QR 스캔",
    [STR_SETTINGS_HINT]     = "이동  조정  A: 선택  B: 뒤로",

    [STR_YES]               = "예",
    [STR_NO]                = "아니오",
    [STR_CONFIRM]           = "확인",
    [STR_DELETE_FMT]        = "「%s」를 삭제하시겠습니까?",
    [STR_RETURN_TO_MENU]    = "메뉴로 돌아가시겠습니까?",

    [STR_WIFI_CONNECTING]   = "WiFi 연결 중...",
    [STR_WIFI_CONNECTED]    = "연결되었습니다!",
    [STR_WIFI_FAILED]       = "WiFi 연결 실패",
    [STR_DOWNLOADING]       = "다운로드 중...",
    [STR_DL_COMPLETE]       = "다운로드 완료!",
    [STR_DL_FAILED]         = "다운로드 실패",
    [STR_LOADING]           = "로딩 중...",
    [STR_LOAD_FAILED]       = "로드 실패",
    [STR_STARTING]          = "시작!",
    [STR_NO_WIFI]           = "WiFi 없음",
    [STR_NO_WIFI_DETAIL]    = "설정에서 WiFi를 먼저 설정하세요",
    [STR_BUTTON_MAP]        = "버튼 배치",
    [STR_BUTTON_MAP_DETAIL] = "방향키:화살표 A:스페이스",
    [STR_BUTTON_MAP_HINT]   = "B: 닫기",
    [STR_BUTTON_MAP_NONE]   = "이 게임은 버튼을 사용하지 않아요",
    [STR_FETCHING_INFO]     = "정보 가져오는 중...",
    [STR_LOADING_JSON]      = "JSON 로딩 중...",
    [STR_ASSETS]            = "에셋",
    [STR_LOADING_SOUNDS]    = "사운드 로딩 중...",
    [STR_LOADING_COSTUMES]  = "코스튬 로딩 중...",

    [STR_QR_SCAN_WIFI_HEAD]    = "WiFi QR을 보여줘!",
    [STR_QR_SCAN_PROJECT_HEAD] = "게임 QR을 보여줘!",
    [STR_QR_HINT_BACK]         = "B: 뒤로",
    [STR_WIFI_TRY_AGAIN]       = "다시 해보자",
};

static const char *ZH_CN[STR_COUNT] = {
    [STR_FOOTER_HINT]       = "A: 选择  B: 返回",
    [STR_GAMEPAD_OK]        = " 手柄",
    [STR_GAMEPAD_NONE]      = " 未连接",

    [STR_MAIN_GAMES]        = "游戏列表",
    [STR_MAIN_NEW_GAME]     = "新游戏 (QR)",
    [STR_MAIN_SETTINGS]     = "设置",

    [STR_GAMES_TITLE]       = "已保存的游戏",
    [STR_BACK]              = "返回",
    [STR_GAMES_EMPTY]       = "没有保存的游戏。\n请从「新游戏」下载。",
    [STR_GAMES_HINT]        = "A: 开始  X: 删除  B: 返回",

    [STR_SETTINGS_TITLE]    = "设置",
    [STR_BRIGHTNESS]        = "亮度",
    [STR_MUTE_AUDIO]        = "静音",
    [STR_VOLUME]            = "音量",
    [STR_LEVEL_LOW]         = "小",
    [STR_LEVEL_HIGH]        = "大",
    [STR_LANGUAGE]          = "语言",
    [STR_NETWORK]           = "网络",
    [STR_WIFI_SAVED_FMT]    = "已保存: %s",
    [STR_WIFI_NONE]         = "未配置 WiFi",
    [STR_WIFI_SCAN_QR]      = "扫描 WiFi QR",
    [STR_SETTINGS_HINT]     = "移动  调整  A: 选择  B: 返回",

    [STR_YES]               = "是",
    [STR_NO]                = "否",
    [STR_CONFIRM]           = "确认",
    [STR_DELETE_FMT]        = "删除「%s」？",
    [STR_RETURN_TO_MENU]    = "返回主菜单？",

    [STR_WIFI_CONNECTING]   = "正在连接 WiFi...",
    [STR_WIFI_CONNECTED]    = "已连接！",
    [STR_WIFI_FAILED]       = "WiFi 连接失败",
    [STR_DOWNLOADING]       = "下载中...",
    [STR_DL_COMPLETE]       = "下载完成！",
    [STR_DL_FAILED]         = "下载失败",
    [STR_LOADING]           = "加载中...",
    [STR_LOAD_FAILED]       = "加载失败",
    [STR_STARTING]          = "开始！",
    [STR_NO_WIFI]           = "无 WiFi",
    [STR_NO_WIFI_DETAIL]    = "请先在设置中配置 WiFi",
    [STR_BUTTON_MAP]        = "按键映射",
    [STR_BUTTON_MAP_DETAIL] = "方向键:方向 A:空格",
    [STR_BUTTON_MAP_HINT]   = "B: 关闭",
    [STR_BUTTON_MAP_NONE]   = "这个游戏不使用按键",
    [STR_FETCHING_INFO]     = "获取信息中...",
    [STR_LOADING_JSON]      = "加载 JSON 中...",
    [STR_ASSETS]            = "资源",
    [STR_LOADING_SOUNDS]    = "加载声音中...",
    [STR_LOADING_COSTUMES]  = "加载造型中...",

    [STR_QR_SCAN_WIFI_HEAD]    = "把 WiFi QR 给我看看！",
    [STR_QR_SCAN_PROJECT_HEAD] = "把游戏 QR 给我看看！",
    [STR_QR_HINT_BACK]         = "B: 返回",
    [STR_WIFI_TRY_AGAIN]       = "再试一次",
};

static const char *lang_to_code(Lang l) {
    switch (l) {
        case Lang::JP:    return "jp";
        case Lang::KR:    return "ko";
        case Lang::ZH_CN: return "zh_cn";
        case Lang::EN:    /* fallthrough */
        default:          return "en";
    }
}

static Lang code_to_lang(const char *code) {
    if (!code) return Lang::EN;
    if (strcmp(code, "jp") == 0)    return Lang::JP;
    if (strcmp(code, "ko") == 0)    return Lang::KR;
    if (strcmp(code, "zh_cn") == 0) return Lang::ZH_CN;
    return Lang::EN;
}

void lang_init()
{
    char buf[8];
    if (sd_load_lang(buf, sizeof(buf))) {
        s_lang = code_to_lang(buf);
        ESP_LOGI(TAG, "Loaded language: %s", buf);
    }
}

Lang lang_get() { return s_lang; }

void lang_set(Lang l)
{
    s_lang = l;
    sd_save_lang(lang_to_code(l));
}

const char *tr(StringId id)
{
    if (id < 0 || id >= STR_COUNT) return "";
    const char *const *table;
    switch (s_lang) {
        case Lang::JP:    table = JP; break;
        case Lang::KR:    table = KR; break;
        case Lang::ZH_CN: table = ZH_CN; break;
        case Lang::EN:    /* fallthrough */
        default:          table = EN; break;
    }
    const char *s = table[id];
    // Fall back to EN if a translation entry is missing for this language.
    if (!s) s = EN[id];
    return s ? s : "";
}
