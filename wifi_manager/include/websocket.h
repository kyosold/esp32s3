#ifndef _WEBSOCKET_H_
#define _WEBSOCKET_H_

#include "esp_err.h"

// 回调函数: 用于接收收到的消息
typedef void (*web_ws_receive_cb_t)(uint8_t *data, int len);

typedef struct {
    const char *html_code;
    web_ws_receive_cb_t receive_callback;
} ws_cfg_t;

esp_err_t web_ws_start(ws_cfg_t *cfg);

esp_err_t web_ws_stop(void);

esp_err_t web_ws_send(uint8_t *data, int len);


#endif