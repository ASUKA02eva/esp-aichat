# WebSocket 客户端（与大模型服务端双向通信）
idf_component_register(SRCS "websocket_client.cc"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_websocket_client)
