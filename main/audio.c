/* audio.c — ADC1 continuous capture + on-device FFT + WS streaming.
 *
 * Wire a crude electret-mic-+-bias-network into ADC_MIC_GPIO (defined per
 * board in probe.h). The S3 ADC continuous driver feeds a ring buffer; a
 * pump task pulls a fixed-size window, runs a real-valued FFT via esp-dsp,
 * converts to magnitude bins (dB), and pushes a binary frame to whichever
 * client is attached to /audio/ws.
 *
 * Audio regressions in the smoke suite work like this: TC8 plays a known
 * test tone (e.g. 1 kHz square via aplay), brainslug reads the FFT and the
 * smoke harness asserts a peak at 1 kHz ± a tolerance.
 *
 * Frame format on /audio/ws (binary, little-endian):
 *   u32  sample_rate_hz
 *   u32  fft_bins                (N = AUDIO_FFT_SIZE / 2)
 *   i16  magnitudes_dB[N]        signed Q8.8, range roughly [-128.0, +127.99]
 *
 * Browser side: parse the header, render N bars. Bin i corresponds to
 * frequency i * sample_rate_hz / AUDIO_FFT_SIZE.
 */
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_continuous.h"
#include "esp_http_server.h"
#include "esp_dsp.h"
#include "probe.h"

#if CONFIG_PROBE_AUDIO

static const char *TAG = "audio";

#define ADC_SAMPLE_HZ     16000
#define AUDIO_FFT_SIZE    1024          /* must match dsps_fft_init order */
#define AUDIO_FFT_BINS    (AUDIO_FFT_SIZE / 2)

static adc_continuous_handle_t s_adc = NULL;
static httpd_handle_t s_ws_srv = NULL;
static int s_ws_fd = -1;
static TaskHandle_t s_pump_task = NULL;

/* Real-valued FFT plan + scratch (esp-dsp wants float, dB output is int16 Q8.8). */
static __attribute__((aligned(16))) float s_window[AUDIO_FFT_SIZE];
static __attribute__((aligned(16))) float s_fft_buf[AUDIO_FFT_SIZE * 2];  /* complex */
static int16_t s_mag_db[AUDIO_FFT_BINS];

static void hann_window_init(void)
{
    for (int i = 0; i < AUDIO_FFT_SIZE; ++i) {
        s_window[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (AUDIO_FFT_SIZE - 1)));
    }
}

/* Push one FFT frame to whichever WS client is attached. Drops silently if
 * no client. */
static void ws_send_frame(void)
{
    if (s_ws_fd < 0 || !s_ws_srv) return;

    /* header (8 B) + i16 bins */
    uint8_t hdr[8];
    uint32_t sr = ADC_SAMPLE_HZ, n = AUDIO_FFT_BINS;
    memcpy(hdr,     &sr, 4);
    memcpy(hdr + 4, &n,  4);

    size_t payload = sizeof hdr + sizeof s_mag_db;
    uint8_t *buf = malloc(payload);
    if (!buf) return;
    memcpy(buf, hdr, sizeof hdr);
    memcpy(buf + sizeof hdr, s_mag_db, sizeof s_mag_db);

    httpd_ws_frame_t f = {
        .final = true, .fragmented = false, .type = HTTPD_WS_TYPE_BINARY,
        .payload = buf, .len = payload,
    };
    httpd_ws_send_frame_async(s_ws_srv, s_ws_fd, &f);
    free(buf);
}

static void audio_pump_task(void *arg)
{
    uint8_t rxbuf[AUDIO_FFT_SIZE * sizeof(adc_digi_output_data_t)];
    while (1) {
        if (s_ws_fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        uint32_t got = 0;
        if (adc_continuous_read(s_adc, rxbuf, sizeof rxbuf, &got, 500) != ESP_OK || !got) continue;

        /* Each adc_digi_output_data_t holds one 12-bit sample. Reshape into a
         * window of floats, Hann-windowed, into the complex FFT buffer (real
         * part only — esp-dsp dsps_fft2r_fc32 expects interleaved RE/IM). */
        int n_samples = got / sizeof(adc_digi_output_data_t);
        if (n_samples > AUDIO_FFT_SIZE) n_samples = AUDIO_FFT_SIZE;
        adc_digi_output_data_t *p = (adc_digi_output_data_t *)rxbuf;
        for (int i = 0; i < n_samples; ++i) {
            float v = (float)p[i].type1.data - 2048.0f;  /* center */
            s_fft_buf[2 * i]     = v * s_window[i];
            s_fft_buf[2 * i + 1] = 0.0f;
        }
        for (int i = n_samples; i < AUDIO_FFT_SIZE; ++i) {
            s_fft_buf[2 * i] = 0.0f; s_fft_buf[2 * i + 1] = 0.0f;
        }

        dsps_fft2r_fc32(s_fft_buf, AUDIO_FFT_SIZE);
        dsps_bit_rev_fc32(s_fft_buf, AUDIO_FFT_SIZE);

        /* Magnitude in dB, Q8.8 int16. */
        for (int i = 0; i < AUDIO_FFT_BINS; ++i) {
            float re = s_fft_buf[2 * i], im = s_fft_buf[2 * i + 1];
            float mag2 = re * re + im * im;
            float db = (mag2 > 1e-9f) ? 10.0f * log10f(mag2) : -90.0f;
            int v = (int)(db * 256.0f);
            if (v >  32767) v =  32767;
            if (v < -32768) v = -32768;
            s_mag_db[i] = (int16_t)v;
        }

        ws_send_frame();
    }
}

esp_err_t audio_init(void)
{
    hann_window_init();

    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, AUDIO_FFT_SIZE));

    adc_continuous_handle_cfg_t hcfg = {
        .max_store_buf_size = 4096,
        .conv_frame_size    = 256,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&hcfg, &s_adc));

    adc_digi_pattern_config_t pat = {
        .atten     = ADC_ATTEN_DB_12,
        .channel   = ADC_MIC_CHANNEL,
        .unit      = ADC_UNIT_1,
        .bit_width = ADC_BITWIDTH_12,
    };
    adc_continuous_config_t ccfg = {
        .sample_freq_hz = ADC_SAMPLE_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
        .pattern_num    = 1,
        .adc_pattern    = &pat,
    };
    ESP_ERROR_CHECK(adc_continuous_config(s_adc, &ccfg));
    ESP_ERROR_CHECK(adc_continuous_start(s_adc));

    xTaskCreate(audio_pump_task, "audio-pump", 8192, NULL, 5, &s_pump_task);
    ESP_LOGI(TAG, "audio init: %d Hz, FFT %d, ADC1 ch %d",
             ADC_SAMPLE_HZ, AUDIO_FFT_SIZE, ADC_MIC_CHANNEL);
    return ESP_OK;
}

static esp_err_t audio_ws_handler(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {
        s_ws_srv = r->handle;
        s_ws_fd  = httpd_req_to_sockfd(r);
        ESP_LOGI(TAG, "audio ws sink fd=%d", s_ws_fd);
        return ESP_OK;
    }
    /* No inbound frames expected; we only stream. */
    return ESP_OK;
}

void audio_register_routes(httpd_handle_t srv)
{
    static httpd_uri_t ws = {
        .uri = "/audio/ws", .method = HTTP_GET,
        .handler = audio_ws_handler, .user_ctx = NULL,
        .is_websocket = true, .handle_ws_control_frames = false,
    };
    httpd_register_uri_handler(srv, &ws);
}

#endif /* CONFIG_PROBE_AUDIO */
