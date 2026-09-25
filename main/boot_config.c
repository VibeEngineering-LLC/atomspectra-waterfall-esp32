#include "boot_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#include <inttypes.h>   // PRIu32 в журнале сессии (issue #52)

static const char *TAG = "bootcfg";
#define BOOT_NS "boot"

static bool get_flag(nvs_handle_t h, const char *key)
{
    uint8_t v = 0;
    return (nvs_get_u8(h, key, &v) == ESP_OK) && (v != 0);
}

// #FW-42: санитизация префикса — только [A-Za-z0-9_-], усечение до cap-1.
// Гарантирует, что в NVS (и дальше в Content-Disposition) не попадёт мусор,
// ломающий заголовок или имя файла на ФС. dst всегда 0-терминируется.
static void sanitize_prefix(char *dst, size_t cap, const char *src)
{
    size_t j = 0;
    if (src) {
        for (size_t i = 0; src[i] && j + 1 < cap; i++) {
            char c = src[i];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-') {
                dst[j++] = c;
            }
        }
    }
    dst[j] = '\0';
}

void boot_config_load(boot_config_t *out)
{
    if (!out) return;
    out->autostart_spectrum  = false;
    out->autostart_waterfall = false;
    out->clear_spectrum      = false;
    out->clear_waterfall     = false;
    out->name_prefix[0]      = '\0';
    out->backup_keep         = 0;    // issue #52: из коробки выключено
    out->backup_hours        = 24;
    out->backup_test_minutes = false;
    out->field_ap_fallback_disabled = false;   // AWF-2a: по умолчанию ВКЛ

    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READONLY, &h) != ESP_OK) return;   // namespace ещё нет → все false / ""
    out->autostart_spectrum  = get_flag(h, "as_spec");
    out->autostart_waterfall = get_flag(h, "as_wf");
    out->clear_spectrum      = get_flag(h, "clr_spec");
    out->clear_waterfall     = get_flag(h, "clr_wf");
    size_t plen = sizeof(out->name_prefix);
    if (nvs_get_str(h, "nprefix", out->name_prefix, &plen) != ESP_OK) {
        out->name_prefix[0] = '\0';                             // ключа нет → без префикса
    }
    // issue #52. Значения из NVS зажимаются здесь, а не только на входе API:
    // ключ мог быть записан прошивкой с другими пределами или испорчен, а
    // планировщик обязан получить рабочее число в любом случае.
    uint8_t  keep = 0;
    uint16_t hrs  = 0;
    if (nvs_get_u8(h, "bk_n", &keep) == ESP_OK) {
        // Значение вне диапазона (порча ячейки, чужая прошивка) зажимается ВНИЗ,
        // к «выключено». Зажим вверх включил бы максимальную ротацию на плате,
        // где владелец фичу не включал, — для параметра с дефолтом OFF это
        // неверная сторона отказа.
        out->backup_keep = (keep > BOOT_BACKUP_KEEP_MAX) ? 0 : keep;
        if (keep > BOOT_BACKUP_KEEP_MAX)
            ESP_LOGW(TAG, "bk_n=%u out of range — backups treated as OFF", (unsigned)keep);
    }
    if (nvs_get_u16(h, "bk_h", &hrs) == ESP_OK && hrs >= BOOT_BACKUP_HOURS_MIN)
        out->backup_hours = (hrs > BOOT_BACKUP_HOURS_MAX) ? BOOT_BACKUP_HOURS_MAX : hrs;
    out->backup_test_minutes = get_flag(h, "bk_tm");
    out->field_ap_fallback_disabled = get_flag(h, "fap_dis");
    nvs_close(h);
}

int boot_config_save(const boot_config_t *in)
{
    if (!in) return -1;
    char clean[BOOT_NAME_PREFIX_CAP];
    sanitize_prefix(clean, sizeof(clean), in->name_prefix);
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READWRITE, &h) != ESP_OK) return -1;
    esp_err_t e = ESP_OK;
    e |= nvs_set_u8(h, "as_spec",  in->autostart_spectrum  ? 1 : 0);
    e |= nvs_set_u8(h, "as_wf",    in->autostart_waterfall ? 1 : 0);
    e |= nvs_set_u8(h, "clr_spec", in->clear_spectrum      ? 1 : 0);
    e |= nvs_set_u8(h, "clr_wf",   in->clear_waterfall     ? 1 : 0);
    e |= nvs_set_str(h, "nprefix", clean);
    // issue #52. Зажим повторяется на записи: HTTP-слой уже проверил диапазон и
    // вернул 400, но boot_config_save() вызывается и из других мест, а неверный
    // период в NVS переживёт перезагрузку и будет тихо работать месяцами.
    {
        uint8_t  keep = (in->backup_keep > BOOT_BACKUP_KEEP_MAX)
                            ? BOOT_BACKUP_KEEP_MAX : in->backup_keep;
        uint16_t hrs  = in->backup_hours;
        if (hrs < BOOT_BACKUP_HOURS_MIN) hrs = BOOT_BACKUP_HOURS_MIN;
        if (hrs > BOOT_BACKUP_HOURS_MAX) hrs = BOOT_BACKUP_HOURS_MAX;
        e |= nvs_set_u8 (h, "bk_n",  keep);
        e |= nvs_set_u16(h, "bk_h",  hrs);
        e |= nvs_set_u8 (h, "bk_tm", in->backup_test_minutes ? 1 : 0);
    }
    e |= nvs_set_u8(h, "fap_dis", in->field_ap_fallback_disabled ? 1 : 0);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) { ESP_LOGW(TAG, "save failed (0x%x)", (int)e); return -1; }
    return 0;
}

// issue #52: счётчик сессий. Отдельный ключ, отдельные open/commit — намеренно
// не в boot_config_save(), чтобы сохранение настроек из UI не двигало нумерацию
// снимков.
uint32_t boot_config_bump_session(uint32_t floor_value)
{
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "session bump: NVS unavailable — backups disabled this boot");
        return 0;      // 0 = «номера нет»; вызывающий обязан не делать снимков
    }
    uint32_t s = 0;
    nvs_get_u32(h, "sess", &s);      // ключа нет → 0, первая сессия станет 1
    if (s < floor_value) {
        // NVS был стёрт (штатное восстановление IDF при NO_FREE_PAGES), а снимки
        // прежних сессий лежат на flash. Продолжаем с их максимума, иначе новые
        // снимки получили бы МЕНЬШИЙ номер и ротация вытесняла бы именно их.
        ESP_LOGW(TAG, "session counter %" PRIu32 " < snapshots on flash (%" PRIu32
                      ") — NVS was likely erased, continuing from flash",
                 s, floor_value);
        s = floor_value;
    }
    s++;
    esp_err_t e = nvs_set_u32(h, "sess", s);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        // Запись не удалась → следующая загрузка прочитает старое значение и
        // возьмёт ТОТ ЖЕ номер, а `fopen(..,"wb")` перезапишет снимки этой
        // загрузки. Лучше не снимать вовсе, чем затирать уже снятое.
        ESP_LOGE(TAG, "session bump: NVS write failed (0x%x) — backups disabled this boot",
                 (int)e);
        return 0;
    }
    ESP_LOGI(TAG, "board session #%" PRIu32, s);
    return s;
}

uint32_t boot_config_get_session(void)
{
    nvs_handle_t h;
    if (nvs_open(BOOT_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t s = 0;
    nvs_get_u32(h, "sess", &s);
    nvs_close(h);
    return s;
}
