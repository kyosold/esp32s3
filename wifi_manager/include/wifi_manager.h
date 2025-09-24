#ifndef _WIFI_MANAGER_H_
#define _WIFI_MANAGER_H_

#include "esp_err.h"
#include "esp_wifi.h"

typedef enum {
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
} wifi_state_t;

typedef void (*wifi_state_cb)(wifi_state_t state);

void wifi_manager_init(wifi_state_cb f);

// 连接指定的AP热点
void wifi_manager_connect(const char *ssid, const char *password);

// 进入AP模式
esp_err_t wifi_manager_ap(void);

// 扫描附近AP热点
// 由于扫描是阻塞的，所以需要在其他任务中调用，等扫描结束，在回调函数中通知我们
typedef void(*wifi_scan_cb)(int num, wifi_ap_record_t *ap_records); // 回调函数原型
esp_err_t wifi_manager_scan(wifi_scan_cb callback);


#endif
