#include "websocket.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_server.h"

static const char *http_html = NULL;
static web_ws_receive_cb_t ws_receive_cb = NULL;
static httpd_handle_t server_handle = NULL;
static int ws_client_fd = -1;

#define TAG "websocket"

esp_err_t web_get_handler(httpd_req_t *req)
{
    return httpd_resp_send(req, http_html, strlen(http_html));
}
esp_err_t ws_get_handler(httpd_req_t *req)
{
    esp_err_t ret;

    if (req->method == HTTP_GET) {
        ws_client_fd = httpd_req_to_sockfd(req); // 获取客户端连接
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // 当参数3为0时，httpd_ws_recv_frame()会填充数据长度，但不会赋值到data
    // 目的是获取数据的长度
    ret = httpd_ws_recv_frame(req, &ws_pkt, 0); // 获取客户端连接
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t *buf = (uint8_t *)malloc(ws_pkt.len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ws_pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len); // 获取客户端数据
    if (ret == ESP_OK) {
        if (ws_pkt.type == HTTPD_WS_TYPE_TEXT) {
            ESP_LOGI(TAG, "recv websocket data: %s", ws_pkt.payload);
            if (ws_receive_cb)
                ws_receive_cb(ws_pkt.payload, ws_pkt.len);
        } 
    }
    free(buf);
    return ESP_OK;
}
esp_err_t web_ws_start(ws_cfg_t *cfg)
{
    if (cfg == NULL)
        return ESP_FAIL;

    http_html = cfg->html_code;
    ws_receive_cb = cfg->receive_callback;

    // HTTPD 初始化
    httpd_config_t server_cfg = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server_handle, &server_cfg);

    httpd_uri_t uri_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = web_get_handler,
    };
    httpd_register_uri_handler(server_handle, &uri_get);

    httpd_uri_t uri_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_get_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server_handle, &uri_ws);

    return ESP_OK;
}

esp_err_t web_ws_stop(void)
{
    if (server_handle) {
        httpd_stop(server_handle);
        server_handle = NULL;
    }
    return ESP_OK;
}

esp_err_t web_ws_send(uint8_t *data, int len)
{
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    ws_pkt.payload = data;
    ws_pkt.len = len;
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    return httpd_ws_send_data(
                server_handle, 
                ws_client_fd,
                &ws_pkt
            );
}