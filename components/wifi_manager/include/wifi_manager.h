#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include <string>

// STA 模式连接管理。事件组、重试计数与 IP 均为静态成员，进程内只应存在一个有效实例。
class WiFiManager {
public:
    // max_retry：连续失败达到该次数后置 WIFI_FAIL_BIT，connect() 返回失败
    WiFiManager(const std::string& ssid, const std::string& password, int max_retry = 5);
    
    // 已连接状态下析构会自动 disconnect()
    ~WiFiManager();
    
    /**
     * @brief 初始化驱动并连接，阻塞至取得 IP 或重试耗尽。
     * @return ESP_OK 已连通；其余为失败，可能是重试耗尽或默认事件循环创建失败
     */
    esp_err_t connect();
    
    // 停止驱动并释放事件组，返回后对象可再次 connect()
    void disconnect();
    
    /**
     * @return 读事件组标志位；断线后该位不会被清除，需配合上层判断
     */
    bool isConnected() const;
    
    /**
     * @return 点分十进制 IPv4 地址；未连接时返回空串
     */
    std::string getIpAddress() const;
    
    /**
     * @return 当前 AP 的 RSSI，单位 dBm，约 -100 ~ -30，越接近 0 越强；未连接或查询失败返回 0
     */
    int8_t getRssi() const;

private:
    // 运行在系统事件任务上下文，不要在其中做阻塞操作
    static void event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data);
    
    std::string ssid_;
    std::string password_;
    int max_retry_;
    
    static EventGroupHandle_t s_wifi_event_group;
    static int s_retry_num;
    static const int WIFI_CONNECTED_BIT = BIT0;
    static const int WIFI_FAIL_BIT = BIT1;
    
    bool initialized_;
    
    esp_event_handler_instance_t instance_any_id_;
    esp_event_handler_instance_t instance_got_ip_;
    
    static esp_ip4_addr_t s_ip_addr;
};

#endif // WIFI_MANAGER_H