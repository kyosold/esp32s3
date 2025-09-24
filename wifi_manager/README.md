# 使用 WiFi 组件
`wifi_manager` 有两种功能：
1. sta模式，把 `esp32s3` 设备当成终端，连接到指定的wifi网络，然后上网。
2. ap+sta模式, `esp32s3` 设备先当成 AP, 其它终端设备(手机、平板、电脑)连接此 AP 后，选择附近的 AP 上网，**用于配网**

## 使用方法

### 1. STA 模式

#### 1.1 main.c
```c
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include "wifi_manager.h"
#include "esp_log.h"
#include "nvs_flash.h"

#define TAG "main"

#define WIFI_SSID "WIFI_SSID"
#define WIFI_PASSWORD "WIFI_PASSWORD"


void wifi_state_callback(wifi_state_t state)
{
    ESP_LOGI(TAG, "wifi state: %d", state);
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // 启动 wifi
    wifi_manager_init(wifi_state_callback);
    wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD);
}
```

#### 1.2 CMakeLists.txt
```
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        "wifi_manager"
        "nvs_flash"
)
```

### 2. AP+Sta 配网

#### 2.1 main.c
```c
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include "wifi_manager.h"
#include "websocket.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "main"

static char *html_code = NULL;
static char *current_ssid[32];
static char *current_pass[64];

static EventGroupHandle_t ap_ev;
#define AP_EVENT_BIT (BIT0)

/////////////////////////////////////////////
#define SPIFFS_MOUNT    "/spiffs"   // SPIFFS 文件系统挂载点
#define HTML_FILE_PATH  SPIFFS_MOUNT"/apcfg.html" // HTML文件路径
static char *init_web_page_buffer()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SPIFFS_MOUNT,  // 挂载点
        .format_if_mount_failed = false, // 如果格式化失败，则不格式化
        .max_files = 5,             // 最大文件数
        .partition_label = "html",  // 分区表的名字
    };
    esp_vfs_spiffs_register(&conf);

    // 使用标准的IO进行文件操作
    struct stat st;
    if (stat(HTML_FILE_PATH, &st)) {    // 获取文件大小
        ESP_LOGE(TAG, "file not exist");
        return NULL;
    } 
    char *buf = (char *)malloc(st.st_size + 1); // 分配内存
    if (buf == NULL) {
        ESP_LOGE(TAG, "malloc fail");
        return NULL;
    }
    memset(buf, 0, st.st_size + 1);

    FILE *fp = fopen(HTML_FILE_PATH, "r");
    if (fp == NULL) {
        ESP_LOGE(TAG, "open file(%s) fail", HTML_FILE_PATH);
        free(buf);
        return NULL;
    }
    fread(buf, st.st_size, 1, fp);
    fclose(fp);

    return buf;
}

void wifi_state_callback(wifi_state_t state)
{
    ESP_LOGI(TAG, "wifi state: %d", state);
}

void wifi_scan_handle(int num, wifi_ap_record_t *ap_records)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *wifilist_js = cJSON_AddArrayToObject(root, "wifi_list");
    for (int i=0; i<num; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", (char *)ap_records[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", ap_records[i].rssi);
        if (ap_records[i].authmode == WIFI_AUTH_OPEN)
            cJSON_AddBoolToObject(item, "encrypted", false);
        else
            cJSON_AddBoolToObject(item, "encrypted", true);

        cJSON_AddItemToArray(wifilist_js, item);
    }
    char *json_str = cJSON_Print(root);
    ESP_LOGI(TAG, "send data: %s", json_str);

    web_ws_send((uint8_t *)json_str, strlen(json_str));

    cJSON_free(json_str);
    cJSON_Delete(root);
}

void web_ws_receive_handle(uint8_t *data, int len)
{
    cJSON *root = cJSON_Parse((char *)data);
    if (root == NULL) {
        ESP_LOGE(TAG, "json parse fail");
        return;
    }

    cJSON *scan_js = cJSON_GetObjectItem(root, "scan");
    cJSON *ssid_js = cJSON_GetObjectItem(root, "ssid");
    cJSON *password_js = cJSON_GetObjectItem(root, "password");

    if (scan_js) {
        char *scan_value = cJSON_GetStringValue(scan_js);
        if (strcmp(scan_value, "start") == 0) {
            // 启动扫描
            wifi_manager_scan(wifi_scan_handle);
        }
    }
    if (ssid_js && password_js) {
        char *ssid_value = cJSON_GetStringValue(ssid_js);
        char *password_value = cJSON_GetStringValue(password_js);
        snprintf((char *)current_ssid, sizeof(current_ssid), "%s", ssid_value);
        snprintf((char *)current_pass, sizeof(current_pass), "%s", password_value);

        // 通知事件
        xEventGroupSetBits(ap_ev, AP_EVENT_BIT);

        // wifi_manager_connect(ssid_value, password_value);
    }
}

static void ap_wifi_task(void *param)
{
    EventBits_t ev;
    while (1) {
        ev = xEventGroupWaitBits(ap_ev, AP_EVENT_BIT, pdTRUE, pdFAIL, pdMS_TO_TICKS(1000 * 10));
        if (ev & AP_EVENT_BIT) {
            web_ws_stop();
            wifi_manager_connect((char *)current_ssid, (char *)current_pass);
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    // 启动 wifi
    wifi_manager_init(wifi_state_callback);
    wifi_manager_ap();

    // 启动 websocket
    // 加载 html 页面 
    html_code = init_web_page_buffer();
    if (html_code == NULL) {
        ESP_LOGE(TAG, "init web page buffer fail");
        return;
    }

    ws_cfg_t ws_cfg = {
        .html_code = html_code,
        .receive_callback = web_ws_receive_handle,
    };
    web_ws_start(&ws_cfg);

    ap_ev = xEventGroupCreate();
    xTaskCreatePinnedToCore(
        ap_wifi_task,
        "ap_wifi_task",
        4096,
        NULL,
        3,
        NULL,
        1
    );
}
```

#### 2.1 CMakeLists.txt
```
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        "wifi_manager"
        "nvs_flash"
        "spiffs"
        "json"
)

spiffs_create_partition_image(
    html 
    ../components/wifi_manager/html 
    FLASH_IN_PROJECT)

```
