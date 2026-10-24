/**
 * expLORA Gateway
 *
 * Smaller network driver buffers (linker wraps)
 *
 * Copyright Pajenicko s.r.o., Igor Sverma (C) 2025
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// The Arduino core builds the driver configurations itself (WiFiGeneric.cpp,
// PPP.cpp) and the prebuilt IDF libraries take their defaults from the
// framework's sdkconfig. The linker redirects calls of the wrapped functions here
// (-Wl,--wrap=..., platformio.ini); only values that cost RAM are lowered,
// everything else is passed on unchanged. scripts/check_wraps.py fails the
// build when a wrap is configured but not linked.

#include <Arduino.h>
#include <esp_wifi.h>
#include "../config.h"

// WiFi: static RX buffers (1.6 kB each, allocated at init) 4 -> 2, dynamic
// RX / TX buffers (allocated per frame) capped at 16 instead of 32, RX
// management buffers 5 -> 3, no ESP-NOW peers (unused), no CSI (unused).
// The block-ack RX window is kept at most the static RX buffers (as the IDF
// recommends; the hardware receives a whole aggregate into them): with 2
// buffers and the default window (Lite 16, Pro 6) uploads to the Lite ran
// ~15 % slower, with a window of 2 as fast as with the stock buffers.
static constexpr int WIFI_STATIC_RX = 2;
static constexpr int WIFI_DYNAMIC_RX = 16;
static constexpr int WIFI_DYNAMIC_TX = 16;
static constexpr int WIFI_RX_MGMT = 3;

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);

extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config)
{
    if (config == nullptr)
    {
        return __real_esp_wifi_init(config);
    }
    wifi_init_config_t cfg = *config;
    cfg.static_rx_buf_num = min(cfg.static_rx_buf_num, WIFI_STATIC_RX);
    cfg.dynamic_rx_buf_num = min(cfg.dynamic_rx_buf_num, WIFI_DYNAMIC_RX);
    cfg.dynamic_tx_buf_num = min(cfg.dynamic_tx_buf_num, WIFI_DYNAMIC_TX);
    cfg.rx_mgmt_buf_num = min(cfg.rx_mgmt_buf_num, WIFI_RX_MGMT);
    cfg.rx_ba_win = min(cfg.rx_ba_win, cfg.static_rx_buf_num);
    cfg.espnow_max_encrypt_num = 0;
    cfg.csi_enable = 0;
    return __real_esp_wifi_init(&cfg);
}

#if defined(HAS_LTE)
#include <esp_modem_c_api_types.h>

// LTE modem (esp_modem UART terminal): RX ring 4096 -> 2048 B (~180 ms at
// 115200 Bd; the terminal task drains it continuously), UART event queue
// 30 -> 10, terminal task stack 4096 -> 3072 B (at most 1.3 kB used)
static constexpr int MODEM_RX_BUFFER = 2048;
static constexpr int MODEM_EVENT_QUEUE = 10;
static constexpr uint32_t MODEM_TASK_STACK = 3072;

extern "C" esp_modem_dce_t *__real_esp_modem_new_dev(esp_modem_dce_device_t module,
                                                     const esp_modem_dte_config_t *dte_config,
                                                     const esp_modem_dce_config_t *dce_config, esp_netif_t *netif);

extern "C" esp_modem_dce_t *__wrap_esp_modem_new_dev(esp_modem_dce_device_t module,
                                                     const esp_modem_dte_config_t *dte_config,
                                                     const esp_modem_dce_config_t *dce_config, esp_netif_t *netif)
{
    if (dte_config == nullptr)
    {
        return __real_esp_modem_new_dev(module, dte_config, dce_config, netif);
    }
    esp_modem_dte_config_t dte = *dte_config;
    dte.uart_config.rx_buffer_size = min(dte.uart_config.rx_buffer_size, MODEM_RX_BUFFER);
    dte.uart_config.event_queue_size = min(dte.uart_config.event_queue_size, MODEM_EVENT_QUEUE);
    dte.task_stack_size = min(dte.task_stack_size, MODEM_TASK_STACK);
    return __real_esp_modem_new_dev(module, &dte, dce_config, netif);
}
#endif
