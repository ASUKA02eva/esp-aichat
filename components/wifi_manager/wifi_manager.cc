/**
 * @file wifi_manager.cc
 * @brief WiFi STA 连接，结果经事件组同步；失败时回滚驱动与事件处理器。
 * ESP32 的 STA 只支持 2.4GHz 频段。
 */

#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <cstring>

static const char *TAG = "WiFiManager";

// 静态成员由所有实例共享，同一进程内只应存在一个已连接的实例
EventGroupHandle_t WiFiManager::s_wifi_event_group = NULL;
int WiFiManager::s_retry_num = 0;
esp_ip4_addr_t WiFiManager::s_ip_addr = {0};

WiFiManager::WiFiManager(const std::string& ssid, const std::string& password, int max_retry)
    : ssid_(ssid), password_(password), max_retry_(max_retry), initialized_(false),
      instance_any_id_(nullptr), instance_got_ip_(nullptr) {
}

WiFiManager::~WiFiManager() {
    if (initialized_) {
        disconnect();
    }
}

void WiFiManager::event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    WiFiManager* wifi_manager = static_cast<WiFiManager*>(arg);
    
    // 驱动启动完成后才会发起首次连接
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    // 密码错误、信号过弱、AP 重启都会走到这里，统一按重试次数处理
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < wifi_manager->max_retry_) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "重连第 %d 次，上限 %d", s_retry_num, wifi_manager->max_retry_);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "WiFi连接失败");
    } 
    // 只有拿到 DHCP 分配的地址才算连通
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        s_ip_addr = event->ip_info.ip;
        ESP_LOGI(TAG, "获得IP地址:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;  // 必须清零，否则下次断开时计数会继续累加
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);  // 唤醒 connect() 中的等待
    }
}

esp_err_t WiFiManager::connect() {
    if (initialized_) {
        ESP_LOGW(TAG, "WiFi已初始化");
        return ESP_OK;
    }
    
    // 事件组由本函数创建，失败路径与 disconnect() 负责删除
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "创建事件组失败");
        return ESP_FAIL;
    }
    
    // 协议栈与应用生命周期同级，只需初始化一次
    ESP_ERROR_CHECK(esp_netif_init());
    
    // 默认事件循环可能已被其它组件创建，INVALID_STATE 视为正常
    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "创建事件循环失败: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // netif 必须先于驱动创建，IP 事件才有承接对象
    esp_netif_create_default_wifi_sta();
    
    // 默认配置中的任务优先级与队列长度不在此处调整
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    // 用 instance 句柄注册，disconnect() 时才能逐条注销
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &event_handler,
                                                       this,
                                                       &instance_any_id_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       IP_EVENT_STA_GOT_IP,
                                                       &event_handler,
                                                       this,
                                                       &instance_got_ip_));
    
    // wifi_config 由驱动拷贝，对象可留在栈上
    wifi_config_t wifi_config = {};
    // ssid 字段定长 32 字节，需留出结尾的 '\0'
    std::strncpy((char*)wifi_config.sta.ssid, ssid_.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    // password 字段定长 64 字节，同样留出 '\0'
    std::strncpy((char*)wifi_config.sta.password, password_.c_str(), sizeof(wifi_config.sta.password) - 1);
    // authmode 是最低要求，加密等级更低的 AP 直接连不上
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    // WPA3_SAE_PWE_BOTH 同时兼容 H2E 与 hunting-and-pecking
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    ESP_LOGI(TAG, "WiFi初始化完成，正在连接 %s", ssid_.c_str());
    
    // 阻塞至两个标志位任一置起，置位发生在系统事件任务上下文
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,  // 两个标志位任一置起即返回
                                          pdFALSE,                             // 不清除，供 isConnected() 读取
                                          pdFALSE,
                                          portMAX_DELAY);                      // 重试耗尽后必然置位，无需超时
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi连接成功: %s", ssid_.c_str());
        initialized_ = true;
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "WiFi连接失败: %s", ssid_.c_str());
        ESP_LOGI(TAG, "请检查 SSID 与密码是否正确");
        
        // 回滚到未初始化状态，否则对象无法再次 connect()
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        esp_wifi_stop();
        esp_wifi_deinit();
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return ESP_FAIL;
    } else {
        ESP_LOGE(TAG, "事件组返回未知标志位");
        return ESP_FAIL;
    }
}

void WiFiManager::disconnect() {
    if (!initialized_) {
        return;
    }
    
    ESP_LOGI(TAG, "正在断开WiFi");
    
    // 先注销再停驱动，避免回调访问已删除的事件组
    if (instance_any_id_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }
    
    // stop 与 deinit 成对，之后如需联网必须重新 connect()
    esp_wifi_stop();
    esp_wifi_deinit();
    
    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    
    // 状态清零后对象可再次 connect()
    initialized_ = false;
    s_retry_num = 0;
    s_ip_addr.addr = 0;
    
    ESP_LOGI(TAG, "WiFi已断开");
}

bool WiFiManager::isConnected() const {
    if (!initialized_ || !s_wifi_event_group) {
        return false;
    }
    
    // 该标志位在断开事件中不会被清除
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

std::string WiFiManager::getIpAddress() const {
    if (!isConnected()) {
        return "";
    }
    
    // 点分十进制地址最长 15 字节，加结尾 '\0' 共 16
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&s_ip_addr));
    return std::string(ip_str);
}

int8_t WiFiManager::getRssi() const {
    if (!isConnected()) {
        return 0;
    }
    
    // 未连接时 esp_wifi_sta_get_ap_info 直接返回失败
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;  // 单位 dBm，约 -100 ~ -30，越接近 0 越强
    }
    return 0;
}