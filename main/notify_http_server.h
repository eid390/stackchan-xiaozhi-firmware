#ifndef NOTIFY_HTTP_SERVER_H
#define NOTIFY_HTTP_SERVER_H

// Notify HTTP server —— 独立于小智 wss 通道，让 Mac 端 hook 能通过局域网
// HTTP POST 直接触发派蒙的"注意力提醒"动作（Claude Code 等确认时用）。
//
// 设计要点：
//  1. 纯 HTTP POST，不是 WebSocket —— Mac 端 curl 就能触发，简单可靠。
//  2. 请求 body 转成 JSON-RPC 2.0 报文 → McpServer::ParseMessage()
//     完全复用 MCP 工具体系；跟 otto-robot 的 ws 桥同思路（otto 已在跑，
//     线程安全在 httpd task 里已被上游验证过）。
//  3. 端口 8788（跟 kindle-monitor 的 8787 岔开，两套独立）。
//  4. URI：POST /notify {"project": "xxx"} / POST /dismiss / GET /healthz
//
// 用法（在 board 构造里）：
//    notify_http_server_ = new NotifyHttpServer();
//    notify_http_server_->Start(8788);

#include <esp_http_server.h>

class NotifyHttpServer {
public:
    NotifyHttpServer();
    ~NotifyHttpServer();

    bool Start(int port = 8788);
    void Stop();

private:
    httpd_handle_t server_handle_;

    // URI handlers（static，通过 user_ctx 拿实例；参照 otto ws_handler 模式）
    static esp_err_t notify_handler(httpd_req_t* req);
    static esp_err_t dismiss_handler(httpd_req_t* req);
    static esp_err_t healthz_handler(httpd_req_t* req);

    // 收到 POST 后把 body 里的 project 名字装进 MCP 报文，触发 self.notify.attention
    void InvokeAttention(const char* project);
    void InvokeDismiss();
};

#endif  // NOTIFY_HTTP_SERVER_H
