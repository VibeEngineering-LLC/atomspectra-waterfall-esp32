#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#define SPECTRUM_CHANNELS     8192
#define SPECTRUM_CHUNK_BINS   64
#define SPECTRUM_CHUNKS       (SPECTRUM_CHANNELS / SPECTRUM_CHUNK_BINS)

#define ANALYZER_VID          0x0403
#define ANALYZER_PID          0x6001
#define ANALYZER_BAUD         600000

#define WIFI_SSID_MAX         32
#define WIFI_PASS_MAX         64

#define TCP_BRIDGE_PORT       8234

// #DEDUP-1: единая точка монтирования LittleFS. Раньше дублировалась локальными
// #define в spectrum.c / spectrogram.c / web_server.c.
#define STORAGE_PATH          "/storage"

#define CMD_HISTOGRAM         0x01
#define CMD_OSCILLOSCOPE      0x02
#define CMD_TEXT              0x03
#define CMD_STAT              0x04
#define CMD_REBOOT            0xF3

#define CALIB_COEFFS          5

typedef struct {
    uint32_t bins[SPECTRUM_CHANNELS];
    uint32_t total_counts;
    uint32_t total_time_sec;
    uint16_t cpu_load;
    uint32_t cps;
    uint32_t lost_impulses;
    uint32_t pulse_width;   // #DT-4: суммарная ширина импульсов (отсчёты АЦП), STAT offset 14 — диагностика; в расчёт МВ не идёт (метод BecqMoni)
    float    temperature[3];
    double   calibration[CALIB_COEFFS];
    int      calib_order;
    char     serial_number[64];
    time_t   saved_at;
    bool     valid;
    bool     calib_valid;
} spectrum_data_t;

typedef struct {
    uint8_t  dev;
    uint8_t  version;
    uint16_t rise;
    uint16_t fall;
    uint16_t srise;
    uint16_t sfall;
    uint16_t noise;
    float    freq;
    uint32_t max_integral;
    uint16_t hyst;
    uint8_t  mode;
    uint8_t  step;
    uint32_t time_sec;
    uint8_t  pot;
    float    t1, t2, t3;
    bool     tc_on;
    uint16_t tp;
    bool     valid;
} device_info_t;

typedef void (*usb_raw_rx_cb_t)(const uint8_t *data, size_t len);

void usb_host_cdc_init(void);
bool usb_host_cdc_is_connected(void);
int  usb_host_cdc_send(const uint8_t *data, size_t len);
void usb_host_cdc_set_raw_rx_cb(usb_raw_rx_cb_t cb);
int  usb_host_send_text_command(const char *cmd);
// #FW-2/#FW-3: настройки автозапуска/очистки при старте платы. Вызвать ОДИН раз
// ДО usb_host_cdc_init() — флаги применяются однократно при первом USB-коннекте.
void usb_host_cdc_set_autostart(bool autostart_spectrum, bool autostart_waterfall, bool clear_spectrum);

void wifi_manager_init(void);
bool wifi_is_connected(void);
bool wifi_manager_is_ap_mode(void);

void web_server_init(void);

void tcp_bridge_init(void);
bool tcp_bridge_client_connected(void);
uint32_t tcp_bridge_dropped_bytes(void);
uint32_t usb_host_cdc_rx_errors(void);  // #TCP-4
void usb_host_cdc_devlog_json(uint32_t since, char *out, size_t outsz);  // #UI-1

void spectrum_init(void);
void spectrum_process_histogram_chunk(const uint8_t *data, size_t len);
void spectrum_process_stat_packet(const uint8_t *data, size_t len);
// #FW-8: счётчики staging-сборки свипов гистограммы (полных commit / отброшенных
// рваных). drops растёт на каждом свипе, побитом flash-freeze finalize+create.
void spectrum_get_hist_stats(uint32_t *commits, uint32_t *drops);
void spectrum_process_info_response(const char *text);
// #DEV-6: ответ на -tc_pot? ("Tcpot [...]") — таблица баз. темп. компенсации,
// НЕ входит в -inf (см. #DOC-3/BUG-AS-08). Хранится сырым текстом для бэкапа.
void spectrum_process_tcpot_response(const char *text);
// Сырые тексты последних ответов -inf / -tc_pot? (байт-в-байт, формат как у
// автосейва MCA.exe) — для /api/settings/backup. *out_seq — монотонный счётчик,
// растёт при каждом новом ответе; используется вызывающим для ожидания свежего
// ответа после отправки команды (сравнить seq до/после).
int spectrum_get_info_raw(char *out, size_t outsz, uint32_t *out_seq);
int spectrum_get_tcpot_raw(char *out, size_t outsz, uint32_t *out_seq);
void spectrum_reset(void);
const spectrum_data_t *spectrum_get_current(void);
bool spectrum_get_snapshot(spectrum_data_t *out);
const device_info_t   *spectrum_get_device_info(void);
int  spectrum_save_to_flash(void);
int  spectrum_load_from_flash(int index, spectrum_data_t *out);
int  spectrum_delete_from_flash(int index);
void spectrum_set_calibration(const double *coeffs, int order);
void spectrum_save_calibration(void);
void spectrum_load_calibration(void);
void spectrum_autosave(void);
void spectrum_restore_autosave(void);
