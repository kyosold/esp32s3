#include "wifi_manager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/ip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *ap_ssid_name = "ESP32S3_AP";
static const char *ap_password = "12345678";

#define MAX_CONNECT_RETRY 5
static int sta_connect_cnt = 0;

// 当前sta连接状态
static bool is_sta_connected = false;

// WIFI 状态回调函数
static wifi_state_cb wifi_callback = NULL;

static esp_netif_t *esp_netif_ap = NULL;

// 二进制信号量，要么有，要么没有
static SemaphoreHandle_t scan_sem = NULL;

#define TAG "wifi_manager"

static void event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                if (is_sta_connected) {
                    is_sta_connected = false;
                    if (wifi_callback) {
                        wifi_callback(WIFI_STATE_DISCONNECTED);
                    }
                }
                if (sta_connect_cnt < MAX_CONNECT_RETRY) {
                    esp_wifi_connect();
                    sta_connect_cnt++;
                }
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Connected to ap");
                break;
            case WIFI_EVENT_AP_STACONNECTED:    // 有设备连接上来了
                ESP_LOGI(TAG, "sta device connected");
                break;
            case WIFI_EVENT_AP_STADISCONNECTED:    // 有设备断开了
                ESP_LOGI(TAG, "sta device disconnected");
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "Get ip addr");
            is_sta_connected = true;
            if (wifi_callback)
                wifi_callback(WIFI_STATE_CONNECTED);
        }
    }
}

void wifi_manager_init(wifi_state_cb f)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    esp_netif_ap = esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &event_handler,
        NULL,
        NULL
    ));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &event_handler,
        NULL,
        NULL
    ));

    wifi_callback = f;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

void wifi_manager_connect(const char *ssid, const char *password)
{
    wifi_config_t config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    snprintf((char *)config.sta.ssid, 32, "%s", ssid);
    snprintf((char *)config.sta.password, 64, "%s", password);

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode != WIFI_MODE_STA) {
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    sta_connect_cnt = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    esp_wifi_start();
}

esp_err_t wifi_manager_ap(void)
{
    // 获取当前模式是否是AP模式
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_APSTA) {
        return ESP_OK;
    }
    esp_wifi_disconnect();
    esp_wifi_stop();

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t wifi_config = {
        .ap = {
            .channel = 5,   // 通信信道，随便
            .max_connection = 5, // 最大连接数
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    snprintf((char *)wifi_config.ap.ssid, 32, "%s", ap_ssid_name);
    wifi_config.ap.ssid_len = strlen(ap_ssid_name);
    snprintf((char *)wifi_config.ap.password, 64, "%s", ap_password);
    esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config);

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 100, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 100, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(esp_netif_ap);
    esp_netif_set_ip_info(esp_netif_ap, &ip_info);
    esp_netif_dhcps_start(esp_netif_ap);

    scan_sem = xSemaphoreCreateBinary();
    xSemaphoreGive(scan_sem);

    return esp_wifi_start();
}

static void scan_task(void *param)
{
    wifi_scan_cb callback = (wifi_scan_cb)param;

    uint16_t ap_count = 0;
    uint16_t ap_num = 20;
    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_num);

    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&ap_count);
    // 获取扫描的结果，
    // 结果数量不会超过 ap_num，
    // 如果少于 ap_num，则返回实际的结果数量
    esp_wifi_scan_get_ap_records(&ap_num, ap_list);
    ESP_LOGI(TAG, "scan ap total:%d, actual number:%d", ap_count, ap_num);

    if (callback) {
        callback(ap_num, ap_list);
    }

    free(ap_list);
    xSemaphoreGive(scan_sem);   // 释放信号量
    vTaskDelete(NULL);  // 释放任务本身占用的系统资源
}
esp_err_t wifi_manager_scan(wifi_scan_cb callback)
{
    // 每次只执行一次扫描
    if (xSemaphoreTake(scan_sem, 0) == pdFAIL) {
        ESP_LOGI(TAG, "scan task is running");
        return ESP_OK;
    }

    // 清除上次扫描信息
    esp_wifi_clear_ap_list();

    return xTaskCreatePinnedToCore(
                scan_task,
                "scan_task",
                8196,
                callback,
                3,
                NULL,
                1       // core 1: app 核
            );
}

