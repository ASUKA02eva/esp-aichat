#!/usr/bin/env python3
"""
ESP32 语音助手 WebSocket 服务器：接收 ESP32 上报的 PCM 音频并转发给
DashScope Omni Realtime 接口，再把模型返回的音频送回 ESP32 播放。

协议（ESP32 -> 服务器，JSON 文本，event 字段）：
    wake_word_detected   唤醒词命中
    recording_started    开始录音，服务器随即建立模型连接
    recording_ended      录音结束，服务器触发模型生成响应
    recording_cancelled  录音作废，丢弃缓冲区

协议（服务器 -> ESP32）：
    二进制帧为 16kHz/单声道/16bit PCM；一轮响应结束后发送一次 ping 作为结束标志。

依赖：websockets、pydub、numpy/scipy（scipy 缺失时退化为线性插值重采样）。
pydub 导出 MP3 需要系统 PATH 中存在 ffmpeg。
前置条件：环境变量 DASHSCOPE_API_KEY；未设置时只保存录音，不请求模型。
监听 0.0.0.0:8888，ESP32 需与本机处于同一网段。
"""

import os
import sys
import json
import base64
import wave
import asyncio
import websockets
from datetime import datetime
from pydub import AudioSegment
import time
import socket

# Omni Realtime 客户端（服务器版实现）
from omni_realtime_client import (
    OmniRealtimeClient,
    TurnDetectionMode,
)

OMNI_CLIENT_AVAILABLE = True

# 音频参数
SAMPLE_RATE = 16000  # ESP32 上行采样率，16kHz
MODEL_SAMPLE_RATE = 24000  # 模型下行采样率，24kHz
CHANNELS = 1  # 单声道
BIT_DEPTH = 16  # 位深，单位 bit
BYTES_PER_SAMPLE = 2  # 每样本字节数：16bit / 8

# WebSocket 服务器配置
WS_HOST = "0.0.0.0"  # 监听所有网络接口
WS_PORT = 8888  # 端口，需在防火墙放行

# 下行音频需从 MODEL_SAMPLE_RATE 重采样到 SAMPLE_RATE，采样率不匹配会改变播放速度


class WebSocketAudioServer:
    """
    WebSocket 音频服务器。

    每条连接维护独立状态：管理连接生命周期、把上行音频转发给模型、
    重采样下行音频后写回 ESP32，并把录音与响应音频落盘。
    """

    def __init__(self):
        """
        创建录音目录并读取 DASHSCOPE_API_KEY。

        密钥缺失时 self.use_model 为 False，服务器仅保存录音、不生成响应。
        """
        # 录音与响应音频的落盘目录
        self.output_dir = os.path.join(
            os.path.dirname(__file__), "user_records"
        )  # 用户录音
        self.response_dir = os.path.join(
            os.path.dirname(__file__), "response_records"
        )  # 模型响应

        os.makedirs(self.output_dir, exist_ok=True)
        os.makedirs(self.response_dir, exist_ok=True)

        # 密钥优先取自环境变量，硬编码方式见下方注释行
        self.api_key = os.environ.get("DASHSCOPE_API_KEY")

        # 硬编码仅用于本地开发，不要把密钥提交到版本控制系统
        # self.api_key = "your-api-key-here"  # 请替换为您的实际 API 密钥

        if not self.api_key or not OMNI_CLIENT_AVAILABLE:
            if not self.api_key:
                print(
                    "警告: 未设置 DASHSCOPE_API_KEY 环境变量\n"
                    "   请通过以下方式之一配置 API 密钥：\n"
                    "   1. 设置环境变量: export DASHSCOPE_API_KEY='your-api-key'\n"
                    "   2. 在代码中硬编码（仅限开发环境）"
                )
            self.use_model = False
        else:
            self.use_model = True
            print("已配置大模型 API，将使用模型生成响应音频")

    async def handle_client(self, websocket, path):
        """
        处理一条客户端连接的完整生命周期。

        文本帧按 event 字段分派；二进制帧只在 is_recording 为真时转发给模型。
        连接结束时取消事件接收任务并关闭模型连接。
        """
        client_ip = websocket.remote_address[0]
        print(f"\n新的客户端连接: {client_ip}")

        # 每条连接独立的状态
        client_state = {
            "is_recording": False,  # 是否正在录音
            "realtime_client": None,  # 模型客户端实例
            "message_task": None,  # 事件接收任务
            "audio_buffer": bytearray(),  # 录音缓冲，用于落盘
            "audio_tracker": {  # 下行发送统计
                "total_sent": 0,  # 累计发送字节数
                "last_time": time.time(),  # 最后一次发送时间
            },
        }

        try:
            async for message in websocket:
                try:
                    # 二进制帧为音频，文本帧为 JSON 控制消息
                    if isinstance(message, bytes):
                        # 仅在录音中转发，其余情况丢弃
                        if (
                            client_state["is_recording"]
                            and client_state["realtime_client"]
                        ):
                            # 同时留一份用于本地录音文件
                            client_state["audio_buffer"].extend(message)

                            # 文本帧只能承载 ASCII，故先做 Base64 编码
                            encoded_data = base64.b64encode(message).decode("utf-8")

                            # 事件名与模型侧追加音频的事件一致
                            event = {
                                "event_id": "event_"
                                + str(int(time.time() * 1000)),  # 事件 ID 取毫秒时间戳
                                "type": "input_audio_buffer.append",  # 追加音频数据
                                "audio": encoded_data,  # Base64 编码的音频
                            }

                            # 发送不阻塞后续音频接收
                            await client_state["realtime_client"].send_event(event)
                            print(f"   转发音频块: {len(message)} 字节")
                        continue

                    # 解析JSON消息
                    data = json.loads(message)
                    event = data.get("event")

                    if event == "wake_word_detected":
                        print(f"[{client_ip}] 检测到唤醒词")

                    elif event == "recording_started":
                        print(f"[{client_ip}] 开始录音")
                        client_state["is_recording"] = True
                        client_state["audio_buffer"] = bytearray()
                        client_state["audio_tracker"] = {
                            "total_sent": 0,
                            "last_time": time.time(),
                        }

                        # 每次录音新建连接，避免沿用上一轮的会话状态
                        if self.use_model:
                            try:
                                # MANUAL 模式下由本服务决定何时请求响应；
                                # on_audio_delta 回调在独立任务中执行，不阻塞事件接收
                                client_state["realtime_client"] = OmniRealtimeClient(
                                    base_url="wss://dashscope.aliyuncs.com/api-ws/v1/realtime",
                                    api_key=self.api_key,
                                    model="qwen-omni-turbo-realtime-2025-05-08",
                                    voice="Chelsie",
                                    # 收到音频片段立即转发，不等整轮响应结束
                                    on_audio_delta=lambda audio: asyncio.create_task(
                                        self.on_audio_delta_handler(
                                            websocket,
                                            client_ip,
                                            audio,
                                            client_state["audio_tracker"],
                                        )
                                    ),
                                    turn_detection_mode=TurnDetectionMode.MANUAL,
                                )

                                await client_state["realtime_client"].connect()

                                client_state["message_task"] = asyncio.create_task(
                                    client_state["realtime_client"].handle_messages()
                                )

                                print(f"[{client_ip}] LLM 连接成功，开始接收实时音频")

                            except Exception as e:
                                print(f"[{client_ip}] 初始化大模型失败: {e}")
                                client_state["realtime_client"] = None

                    elif event == "recording_ended":
                        print(f"[{client_ip}] 录音结束")
                        client_state["is_recording"] = False

                        # 录音结束后：先落盘，再请求模型生成响应
                        if len(client_state["audio_buffer"]) > 0:
                            print(
                                f"[{client_ip}] 音频总大小: {len(client_state['audio_buffer'])} 字节 ({len(client_state['audio_buffer'])/2/SAMPLE_RATE:.2f}秒)"
                            )

                            current_timestamp = datetime.now()
                            saved_file = await self.save_audio(
                                [bytes(client_state["audio_buffer"])], current_timestamp
                            )
                            if saved_file:
                                print(f"[{client_ip}] 音频已保存: {saved_file}")

                        # MANUAL 模式下必须显式请求响应
                        if self.use_model and client_state["realtime_client"]:
                            try:
                                await client_state["realtime_client"].create_response()

                                print(f"[{client_ip}] 等待模型生成响应")
                                max_wait_time = 30  # 超时上限，单位秒
                                start_time = time.time()

                                # 轮询下行发送状态：超过 2 秒无新音频即认为响应结束
                                while time.time() - start_time < max_wait_time:
                                    await asyncio.sleep(0.1)

                                    if (
                                        client_state["audio_tracker"]["total_sent"] > 0
                                        and time.time()
                                        - client_state["audio_tracker"]["last_time"]
                                        > 2.0
                                    ):
                                        print(
                                            f"[{client_ip}] 响应音频发送完成，总计: {client_state['audio_tracker']['total_sent']} 字节"
                                        )
                                        break

                                if client_state["audio_tracker"]["total_sent"] == 0:
                                    print(f"[{client_ip}] 未收到大模型响应")

                                # ping 作为整轮音频结束的标志
                                await websocket.ping()

                            except Exception as e:
                                print(f"[{client_ip}] 模型处理失败: {e}")
                        else:
                            print(f"[{client_ip}] 未启用模型，无法生成响应")

                    elif event == "recording_cancelled":
                        print(f"[{client_ip}] 录音取消")
                        client_state["is_recording"] = False
                        client_state["audio_buffer"] = bytearray()

                except json.JSONDecodeError as e:
                    print(f"[{client_ip}] JSON解析错误: {e}")
                except Exception as e:
                    print(f"[{client_ip}] 处理消息错误: {e}")

        except websockets.exceptions.ConnectionClosed:
            print(f"[{client_ip}] 客户端断开连接")
        except Exception as e:
            print(f"[{client_ip}] 连接错误: {e}")
        finally:
            if client_state["realtime_client"]:
                try:
                    if client_state["message_task"]:
                        client_state["message_task"].cancel()
                    await client_state["realtime_client"].close()
                except:
                    pass

    async def on_audio_delta_handler(
        self, websocket, client_ip, audio_data, audio_tracker
    ):
        """
        模型下行音频回调。

        audio_data 为 24kHz 的 PCM，需重采样到 SAMPLE_RATE 再发送，否则播放速度偏慢；
        发送后更新 audio_tracker 的累计字节数与时间戳。
        """
        try:
            # 模型输出 24kHz，ESP32 只能播放 16kHz
            resampled = self.resample_audio(audio_data, MODEL_SAMPLE_RATE, SAMPLE_RATE)

            await websocket.send(resampled)
            print(f"   -> 流式发送音频块: {len(resampled)} 字节")

            audio_tracker["total_sent"] += len(resampled)
            audio_tracker["last_time"] = time.time()

        except Exception as e:
            print(f"[{client_ip}] 发送音频块失败: {e}")

    async def save_audio(self, audio_buffer, timestamp):
        """
        把录音缓冲写成 user_records/recording_<时间戳>.mp3。

        先写临时 WAV（SAMPLE_RATE/CHANNELS/BIT_DEPTH），再由 pydub 转 MP3 并删除 WAV。
        无数据或转换失败返回 None。
        """
        if not audio_buffer:
            print("没有音频数据可保存")
            return None

        try:
            # 拼接所有音频数据
            audio_data = b"".join(audio_buffer)

            # 生成文件名
            timestamp_str = (
                timestamp.strftime("%Y%m%d_%H%M%S")
                if timestamp
                else datetime.now().strftime("%Y%m%d_%H%M%S")
            )
            wav_filename = os.path.join(
                self.output_dir, f"recording_{timestamp_str}.wav"
            )
            mp3_filename = os.path.join(
                self.output_dir, f"recording_{timestamp_str}.mp3"
            )

            with wave.open(wav_filename, "wb") as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(BIT_DEPTH // 8)
                wav_file.setframerate(SAMPLE_RATE)
                wav_file.writeframes(audio_data)

            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")

            os.remove(wav_filename)

            duration = len(audio_data) / BYTES_PER_SAMPLE / SAMPLE_RATE
            print(f"\n音频信息:")
            print(f"   时长: {duration:.2f} 秒")
            print(f"   大小: {len(audio_data) / 1024:.1f} KB")

            return mp3_filename

        except Exception as e:
            print(f"\n保存音频失败: {e}")
            return None

    async def save_response_audio(
        self, audio_data, timestamp, sample_rate=MODEL_SAMPLE_RATE
    ):
        """把模型响应音频写成 response_records/response_<时间戳>.mp3；采样率由调用方给出。"""
        if not audio_data:
            print("没有响应音频数据可保存")
            return None

        try:
            # 生成文件名
            timestamp_str = (
                timestamp.strftime("%Y%m%d_%H%M%S")
                if timestamp
                else datetime.now().strftime("%Y%m%d_%H%M%S")
            )
            wav_filename = os.path.join(
                self.response_dir, f"response_{timestamp_str}.wav"
            )
            mp3_filename = os.path.join(
                self.response_dir, f"response_{timestamp_str}.mp3"
            )

            with wave.open(wav_filename, "wb") as wav_file:
                wav_file.setnchannels(CHANNELS)
                wav_file.setsampwidth(BIT_DEPTH // 8)
                wav_file.setframerate(sample_rate)
                wav_file.writeframes(audio_data)

            audio = AudioSegment.from_wav(wav_filename)
            audio.export(mp3_filename, format="mp3", bitrate="128k")

            os.remove(wav_filename)

            duration = len(audio_data) / BYTES_PER_SAMPLE / sample_rate
            print(f"\n响应音频信息:")
            print(f"   时长: {duration:.2f} 秒")
            print(f"   大小: {len(audio_data) / 1024:.1f} KB")
            print(f"   采样率: {sample_rate} Hz")

            return mp3_filename

        except Exception as e:
            print(f"\n保存响应音频失败: {e}")
            return None

    def resample_audio(self, audio_data, from_rate, to_rate):
        """
        重采样 PCM 字节流。

        from_rate/to_rate 单位为 Hz，输入输出均为 16bit little-endian 样本。
        优先使用 scipy.signal.resample；未安装 scipy 时退化为线性插值，失真略大。
        两个采样率相同时原样返回。
        """
        if from_rate == to_rate:
            return audio_data

        try:
            import numpy as np
            from scipy import signal

            # 字节流按 int16 little-endian 解释
            audio_array = np.frombuffer(audio_data, dtype=np.int16)

            # 目标样本数由采样率之比决定
            num_samples = int(len(audio_array) * to_rate / from_rate)

            resampled = signal.resample(audio_array, num_samples)

            # 限幅后转回 int16，避免插值过冲
            resampled = np.clip(resampled, -32768, 32767).astype(np.int16)

            return resampled.tobytes()

        except ImportError:
            print("未安装 scipy，改用线性插值重采样")

            # 逐 2 字节解析为有符号 16 位样本
            audio_array = []
            for i in range(0, len(audio_data), 2):
                if i + 1 < len(audio_data):
                    sample = int.from_bytes(
                        audio_data[i : i + 2], byteorder="little", signed=True
                    )
                    audio_array.append(sample)

            # 按比例逐点取样
            ratio = to_rate / from_rate
            resampled = []
            for i in range(int(len(audio_array) * ratio)):
                src_idx = i / ratio
                src_idx_int = int(src_idx)
                src_idx_frac = src_idx - src_idx_int

                if src_idx_int + 1 < len(audio_array):
                    # 线性插值
                    sample = int(
                        audio_array[src_idx_int] * (1 - src_idx_frac)
                        + audio_array[src_idx_int + 1] * src_idx_frac
                    )
                else:
                    sample = audio_array[min(src_idx_int, len(audio_array) - 1)]

                resampled.append(sample)

            # 样本写回 16bit little-endian 字节流
            result = bytearray()
            for sample in resampled:
                result.extend(sample.to_bytes(2, byteorder="little", signed=True))

            return bytes(result)

    def get_local_ips(self):
        """
        返回本机可用于 ESP32 连接的 IPv4 地址列表。

        过滤回环地址，末尾始终追加 127.0.0.1。
        """
        ips = []
        try:
            # 获取主机名
            hostname = socket.gethostname()

            # 枚举各网卡地址
            for info in socket.getaddrinfo(hostname, None):
                # 只保留 IPv4
                if info[0] == socket.AF_INET:
                    ip = info[4][0]
                    if ip not in ips and not ip.startswith("127."):
                        ips.append(ip)

            # 上面的方法取不到时改用 UDP 探测
            if not ips:
                # 连接外部地址不会真正发包，只用于取本机出口 IP
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                try:
                    s.connect(("8.8.8.8", 80))
                    ip = s.getsockname()[0]
                    if ip not in ips and not ip.startswith("127."):
                        ips.append(ip)
                except:
                    pass
                finally:
                    s.close()

            ips.append("127.0.0.1")

        except Exception as e:
            print(f"获取本机IP地址失败: {e}")
            ips = ["127.0.0.1"]

        return ips

    async def start_server(self):
        """
        打印可用连接地址并启动 WebSocket 服务，持续运行直到手动停止。
        """
        print("=" * 60)
        print("ESP32 音频 WebSocket 服务器")
        print("=" * 60)

        local_ips = self.get_local_ips()
        print("可用的连接地址:")
        for ip in local_ips:
            print(f"  - ws://{ip}:{WS_PORT}")

        if self.use_model:
            print(f"\n响应模式: 大模型生成响应")
            print(f"模型: qwen-omni-turbo-realtime")
        else:
            print(f"\n响应模式: 未启用（需要设置 DASHSCOPE_API_KEY）")
            print(f"提示: 设置环境变量 DASHSCOPE_API_KEY 以启用响应生成")
        print("=" * 60)
        print("\n等待ESP32连接...\n")

        async with websockets.serve(self.handle_client, WS_HOST, WS_PORT):
            await asyncio.Future()  # 永远运行


def main():
    """
    程序入口：创建服务器实例并运行事件循环，Ctrl+C 退出。
    """
    server = WebSocketAudioServer()

    try:
        asyncio.run(server.start_server())
    except KeyboardInterrupt:
        print("\n\n服务器已停止")


if __name__ == "__main__":
    main()
