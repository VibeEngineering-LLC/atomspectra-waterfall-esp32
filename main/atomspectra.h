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

void wifi_manager_init(void);
bool wifi_is_connected(void);
bool wifi_manager_is_ap_mode(void);

void web_server_init(void);

void tcp_bridge_init(void);
bool tcp_bridge_client_connected(void);

void spectrum_init(void);
void spectrum_process_histogram_chunk(const uint8_t *data, size_t len);
void spectrum_process_stat_packet(const uint8_t *data, size_t len);
void spectrum_process_info_response(const char *text);
void spectrum_reset(void);
const spectrum_data_t *spectrum_get_current(void);
bool spectrum_get_snapshot(spectrum_data_t *out);
const device_info_t   *spectrum_get_device_info(void);
int  spectrum_save_to_flash(void);
int  spectrum_list_saved(char *buf, size_t buf_size);
int  spectrum_load_from_flash(int index, spectrum_data_t *out);
int  spectrum_delete_from_flash(int index);
void spectrum_set_calibration(const double *coeffs, int order);
void spectrum_save_calibration(void);
void spectrum_load_calibration(void);
void spectrum_autosave(void);
void spectrum_restore_autosave(void);
