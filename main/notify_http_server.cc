#include "notify_http_server.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <cstdlib>
#include <string>

static const char* TAG = "NotifyHttp";

// MCP JSON-RPC 递增 id，避开跟 wss 那一路重叠（wss 那边从小数字起）
static int g_notify_rpc_id = 1000000;

NotifyHttpServer::NotifyHttpServer() : server_handle_(nullptr) {}

NotifyHttpServer::~NotifyHttpServer() {
    Stop();
}

// ── HTTP handlers ──────────────────────────────────────────────────────

esp_err_t NotifyHttpServer::notify_handler(httpd_req_t* req) {
    auto* self = static_cast<NotifyHttpServer*>(req->user_ctx);
    if (self == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
        return ESP_FAIL;
    }

    // 读 body（≤ 512 字节够用；project 名字撑死几十字节）
    if (req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
        return ESP_FAIL;
    }
    char buf[513];
    int total = 0;
    while (total < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + total, req->content_len - total);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        total += r;
    }
    buf[total] = '\0';

    // 解析 body 拿 project；缺失就用 "unknown"
    std::string project = "unknown";
    if (total > 0) {
        cJSON* root = cJSON_Parse(buf);
        if (root != nullptr) {
            cJSON* p = cJSON_GetObjectItem(root, "project");
            if (p && cJSON_IsString(p) && p->valuestring != nullptr) {
                project = p->valuestring;
                // 限制长度，防止屏幕溢出 / MCP payload 过大
                if (project.size() > 40) project = project.substr(0, 40);
            }
            cJSON_Delete(root);
        }
    }

    ESP_LOGI(TAG, "notify: project=%s", project.c_str());
    self->InvokeAttention(project.c_str());

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t NotifyHttpServer::dismiss_handler(httpd_req_t* req) {
    auto* self = static_cast<NotifyHttpServer*>(req->user_ctx);
    if (self == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ctx");
        return ESP_FAIL;
    }
    // 丢弃 body（如果有）
    char sink[64];
    int remaining = req->content_len;
    while (remaining > 0) {
        int r = httpd_req_recv(req, sink, sizeof(sink) < (size_t)remaining ? sizeof(sink) : remaining);
        if (r <= 0) break;
        remaining -= r;
    }

    ESP_LOGI(TAG, "dismiss");
    self->InvokeDismiss();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

esp_err_t NotifyHttpServer::healthz_handler(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"service\":\"stackchan-notify\"}");
    return ESP_OK;
}

// ── 内部：把 HTTP 请求转成 MCP tools/call 报文 ─────────────────────────

void NotifyHttpServer::InvokeAttention(const char* project) {
    // 构造 JSON-RPC 2.0：{"jsonrpc":"2.0","id":N,"method":"tools/call",
    //                    "params":{"name":"self.notify.attention",
    //                              "arguments":{"project":"..."}}}
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", g_notify_rpc_id++);
    cJSON_AddStringToObject(root, "method", "tools/call");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "self.notify.attention");
    cJSON* args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "project", project);
    cJSON_AddItemToObject(params, "arguments", args);
    cJSON_AddItemToObject(root, "params", params);

    McpServer::GetInstance().ParseMessage(root);
    cJSON_Delete(root);
}

void NotifyHttpServer::InvokeDismiss() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(root, "id", g_notify_rpc_id++);
    cJSON_AddStringToObject(root, "method", "tools/call");
    cJSON* params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "name", "self.notify.dismiss");
    cJSON_AddItemToObject(params, "arguments", cJSON_CreateObject());
    cJSON_AddItemToObject(root, "params", params);

    McpServer::GetInstance().ParseMessage(root);
    cJSON_Delete(root);
}

// ── Start / Stop ───────────────────────────────────────────────────────

bool NotifyHttpServer::Start(int port) {
    if (server_handle_ != nullptr) {
        ESP_LOGW(TAG, "already started");
        return true;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.max_open_sockets = 4;
    // ctrl_port 跟 otto 的 32769 岔开，避免两个 board 共存时冲突
    config.ctrl_port = 32770;
    // 加长栈（cJSON + McpServer::ParseMessage 内部会摸 servo/Alert，栈用得凶）
    config.stack_size = 8192;

    if (httpd_start(&server_handle_, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %d", port);
        server_handle_ = nullptr;
        return false;
    }

    httpd_uri_t uri_notify = {
        .uri = "/notify",
        .method = HTTP_POST,
        .handler = notify_handler,
        .user_ctx = this,
    };
    httpd_uri_t uri_dismiss = {
        .uri = "/dismiss",
        .method = HTTP_POST,
        .handler = dismiss_handler,
        .user_ctx = this,
    };
    httpd_uri_t uri_healthz = {
        .uri = "/healthz",
        .method = HTTP_GET,
        .handler = healthz_handler,
        .user_ctx = this,
    };
    httpd_register_uri_handler(server_handle_, &uri_notify);
    httpd_register_uri_handler(server_handle_, &uri_dismiss);
    httpd_register_uri_handler(server_handle_, &uri_healthz);

    ESP_LOGI(TAG, "HTTP notify server started on port %d (POST /notify, POST /dismiss, GET /healthz)", port);
    return true;
}

void NotifyHttpServer::Stop() {
    if (server_handle_ != nullptr) {
        httpd_stop(server_handle_);
        server_handle_ = nullptr;
        ESP_LOGI(TAG, "HTTP notify server stopped");
    }
}
