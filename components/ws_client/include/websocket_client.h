#ifndef WEBSOCKET_CLIENT_H
#define WEBSOCKET_CLIENT_H

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string>
#include <functional>

// 连接、断开与数据事件经 EventCallback 上报，回调在 WebSocket 任务上下文执行。
// 断线后由内部任务按 reconnect_interval_ms_ 周期重启底层客户端。
class WebSocketClient {
public:
    enum class EventType {
        CONNECTED,
        DISCONNECTED,
        DATA_TEXT,      // 文本帧
        DATA_BINARY,    // 二进制帧
        PING,
        PONG,
        ERROR
    };
    
    struct EventData {
        EventType type;
        const uint8_t* data;    // 仅数据事件有效，回调返回后失效
        size_t data_len;
        int op_code;
    };
    
    using EventCallback = std::function<void(const EventData&)>;
    
    // uri 形如 ws://192.168.1.100:8888
    // 默认断线后每 5 秒重连一次
    WebSocketClient(const std::string& uri, bool auto_reconnect = true, 
                   int reconnect_interval_ms = 5000);
    
    // 内部会 disconnect()
    ~WebSocketClient();
    
    void setEventCallback(EventCallback callback);
    
    /**
     * @brief 创建底层客户端并启动，不等待连接建立，结果通过事件回调上报。
     * @return ESP_OK 已启动；ESP_FAIL 客户端创建失败；其它为 esp_websocket_client_start 的返回码
     */
    esp_err_t connect();
    
    // 删除重连任务并销毁底层句柄，重复调用安全
    void disconnect();
    
    /**
     * @param timeout_ms 等待发送完成的超时，单位毫秒
     * @return 实际写入的字节数，未连接或发送失败返回 -1
     */
    int sendText(const std::string& text, int timeout_ms = portMAX_DELAY);
    
    /**
     * @param len 数据字节数
     * @param timeout_ms 等待发送完成的超时，单位毫秒
     * @return 实际写入的字节数，未连接或发送失败返回 -1
     */
    int sendBinary(const uint8_t* data, size_t len, int timeout_ms = portMAX_DELAY);
    
    /**
     * @return ESP_OK 表示接口可用；未连接时返回 ESP_ERR_INVALID_STATE
     */
    esp_err_t sendPing();
    
    // 只反映最近一次连接/断开事件，不做额外探测
    bool isConnected() const { return connected_; }
    
    void setAutoReconnect(bool enable) { auto_reconnect_ = enable; }
    
    // 下一次轮询才生效，重连任务正在延迟时不会立即改变
    void setReconnectInterval(int interval_ms) { reconnect_interval_ms_ = interval_ms; }

private:
    // 事件回调在 WebSocket 任务上下文执行，不要在其中做阻塞操作
    static void websocket_event_handler(void* handler_args, esp_event_base_t base, 
                                      int32_t event_id, void* event_data);
    
    // 断线后按 reconnect_interval_ms_ 周期重新启动底层客户端
    static void reconnect_task(void* arg);
    
    std::string uri_;
    bool auto_reconnect_;
    int reconnect_interval_ms_;
    
    esp_websocket_client_handle_t client_;
    
    bool connected_;
    
    TaskHandle_t reconnect_task_handle_;
    
    EventCallback event_callback_;
    
    // 单次事件的数据上限，超过该值的帧由底层分多次投递
    static constexpr int BUFFER_SIZE = 8192;
    static constexpr int TASK_STACK_SIZE = 8192;
    static constexpr int RECONNECT_TASK_STACK_SIZE = 4096;
};

#endif // WEBSOCKET_CLIENT_H