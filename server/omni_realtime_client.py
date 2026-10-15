# -- coding: utf-8 --

import asyncio
import websockets
import json
import base64
import os
import time

from typing import Optional, Callable, List, Dict, Any
from enum import Enum


class TurnDetectionMode(Enum):
    SERVER_VAD = "server_vad"
    MANUAL = "manual"


class OmniRealtimeClient:
    """
    DashScope Omni Realtime API 的 WebSocket 客户端（服务器版）。

    MANUAL 模式下不启用服务端 VAD，需由调用方显式调用 create_response()；
    文本与音频结果经回调下发，日志不逐 token 打印、不输出完整 API Key。
    """

    def __init__(
        self,
        base_url,
        api_key: str,
        model: str = "",
        voice: str = "Chelsie",
        turn_detection_mode: TurnDetectionMode = TurnDetectionMode.MANUAL,
        on_text_delta: Optional[Callable[[str], None]] = None,
        on_audio_delta: Optional[Callable[[bytes], None]] = None,
        on_interrupt: Optional[Callable[[], None]] = None,
        on_input_transcript: Optional[Callable[[str], None]] = None,
        on_output_transcript: Optional[Callable[[str], None]] = None,
        extra_event_handlers: Optional[
            Dict[str, Callable[[Dict[str, Any]], None]]
        ] = None,
        enable_verbose_logging: bool = False,  # 为 True 时打印全部事件与发送内容，仅调试用
    ):
        self.base_url = base_url
        self.api_key = api_key
        self.model = model
        self.voice = voice
        self.ws = None
        self.on_text_delta = on_text_delta
        self.on_audio_delta = on_audio_delta
        self.on_interrupt = on_interrupt
        self.on_input_transcript = on_input_transcript
        self.on_output_transcript = on_output_transcript
        self.turn_detection_mode = turn_detection_mode
        self.extra_event_handlers = extra_event_handlers or {}
        self.enable_verbose_logging = enable_verbose_logging

        # Track current response state
        self._current_response_id = None
        self._current_item_id = None
        self._is_responding = False
        # Track printing state for input and output transcripts
        self._print_input_transcript = False
        self._output_transcript_buffer = ""
        # Cache system prompt
        self._system_prompt = None
        
        # 收集完整的响应文本
        self._response_text_buffer = ""
        self._input_text_buffer = ""

    def _load_system_prompt(self) -> str:
        """Load system prompt from file, use cache if already loaded."""
        if self._system_prompt is not None:
            return self._system_prompt

        default_prompt = ""
        prompt_file_path = os.path.join(os.path.dirname(__file__), "system_prompt.md")

        try:
            with open(prompt_file_path, "r", encoding="utf-8") as f:
                self._system_prompt = f.read().strip()
                # 不显示文件路径，避免暴露目录结构
                print(f"已加载系统提示词")
        except Exception as e:
            print(f"无法读取系统提示词文件: {e}")
            print(f"   使用默认提示词")
            self._system_prompt = default_prompt

        return self._system_prompt

    async def connect(self) -> None:
        """Establish WebSocket connection with the Realtime API."""
        url = f"{self.base_url}?model={self.model}"
        headers = {"Authorization": f"Bearer {self.api_key}"}
        
        # 隐藏API Key，只显示前后几位
        masked_key = self._mask_api_key(self.api_key)
        print(f"连接到大模型服务")
        print(f"   URL: {self.base_url}")
        print(f"   模型: {self.model}")
        print(f"   API Key: {masked_key}")

        # For compatibility with different websockets versions
        try:
            # Try newer websockets API first
            self.ws = await websockets.connect(url, additional_headers=headers)
        except TypeError:
            # Fallback to older API that uses extra_headers
            self.ws = await websockets.connect(url, extra_headers=headers)

        # Set up default session configuration
        if self.turn_detection_mode == TurnDetectionMode.MANUAL:
            await self.update_session(
                {
                    "modalities": ["text", "audio"],
                    "voice": self.voice,
                    "input_audio_format": "pcm16",
                    "output_audio_format": "pcm16",
                    "input_audio_transcription": {"model": "gummy-realtime-v1"},
                    "turn_detection": None,
                }
            )
        elif self.turn_detection_mode == TurnDetectionMode.SERVER_VAD:
            await self.update_session(
                {
                    "modalities": ["text", "audio"],
                    "voice": self.voice,
                    "input_audio_format": "pcm16",
                    "output_audio_format": "pcm16",
                    "input_audio_transcription": {"model": "gummy-realtime-v1"},
                    "turn_detection": {
                        "type": "server_vad",
                        "threshold": 0.1,
                        "prefix_padding_ms": 500,
                        "silence_duration_ms": 900,
                    },
                }
            )
        else:
            raise ValueError(f"Invalid turn detection mode: {self.turn_detection_mode}")

    def _mask_api_key(self, api_key: str) -> str:
        """只保留 API Key 前后各 4 位，中间以 ... 代替"""
        if not api_key or len(api_key) < 8:
            return "***"
        return f"{api_key[:4]}...{api_key[-4:]}"

    async def send_event(self, event) -> None:
        event["event_id"] = "event_" + str(int(time.time() * 1000))
        
        # 只在详细模式下打印发送的事件
        if self.enable_verbose_logging:
            print(f"Send event: type={event['type']}, event_id={event['event_id']}")
        
        await self.ws.send(json.dumps(event))

    async def update_session(self, config: Dict[str, Any]) -> None:
        """Update session configuration."""
        event = {"type": "session.update", "session": config}
        
        # 简化session配置的显示
        print(f"更新会话配置: 音频格式={config.get('input_audio_format')}, 语音={config.get('voice')}")
        
        await self.send_event(event)

    async def stream_audio(self, audio_chunk: bytes) -> None:
        """Stream raw audio data to the API."""
        # only support 16bit 16kHz mono pcm
        audio_b64 = base64.b64encode(audio_chunk).decode()

        append_event = {"type": "input_audio_buffer.append", "audio": audio_b64}
        await self.send_event(append_event)

    async def create_response(self) -> None:
        """Request a response from the API. Needed when using manual mode."""
        system_prompt = self._load_system_prompt()

        event = {
            "type": "response.create",
            "response": {
                "instructions": system_prompt,
                "modalities": ["text", "audio"],
            },
        }
        
        print("请求生成响应...")
        await self.send_event(event)

    async def cancel_response(self) -> None:
        """Cancel the current response."""
        event = {"type": "response.cancel"}
        await self.send_event(event)

    async def handle_interruption(self):
        """Handle user interruption of the current response."""
        if not self._is_responding:
            return

        print("处理中断")

        # 1. Cancel the current response
        if self._current_response_id:
            await self.cancel_response()

        self._is_responding = False
        self._current_response_id = None
        self._current_item_id = None

    async def handle_messages(self) -> None:
        try:
            async for message in self.ws:
                event = json.loads(message)
                event_type = event.get("type")

                # 只在详细模式下打印所有事件
                if self.enable_verbose_logging and event_type != "response.audio.delta":
                    print(f"event: {event_type}")

                if event_type == "error":
                    print("Error: ", event["error"])
                    continue
                elif event_type == "response.created":
                    self._current_response_id = event.get("response", {}).get("id")
                    self._is_responding = True
                    self._response_text_buffer = ""  # 重置文本缓冲区
                    print("开始生成响应...")
                elif event_type == "response.output_item.added":
                    self._current_item_id = event.get("item", {}).get("id")
                elif event_type == "response.done":
                    self._is_responding = False
                    self._current_response_id = None
                    self._current_item_id = None
                    
                    # 输出完整的响应文本
                    if self._response_text_buffer:
                        print(f"\nAI 响应: {self._response_text_buffer}\n")
                        self._response_text_buffer = ""
                    
                    print("响应生成完成")
                # Handle interruptions
                elif event_type == "input_audio_buffer.speech_started":
                    if self.enable_verbose_logging:
                        print("检测到语音开始")
                    if self._is_responding:
                        print("触发中断处理")
                        await self.handle_interruption()

                    if self.on_interrupt:
                        self.on_interrupt()
                elif event_type == "input_audio_buffer.speech_stopped":
                    if self.enable_verbose_logging:
                        print("检测到语音结束")
                # Handle normal response events
                elif event_type == "response.text.delta":
                    if self.on_text_delta:
                        delta_text = event["delta"]
                        self._response_text_buffer += delta_text  # 收集文本
                        self.on_text_delta(delta_text)
                elif event_type == "response.audio.delta":
                    if self.on_audio_delta:
                        audio_bytes = base64.b64decode(event["delta"])
                        self.on_audio_delta(audio_bytes)
                elif (
                    event_type
                    == "conversation.item.input_audio_transcription.completed"
                ):
                    transcript = event.get("transcript", "")
                    if transcript:
                        print(f"\n用户说: {transcript}\n")
                    if self.on_input_transcript:
                        await asyncio.to_thread(self.on_input_transcript, transcript)
                        self._print_input_transcript = True
                elif event_type == "response.audio_transcript.delta":
                    # 不在这里输出，收集到response.done时再输出
                    if self.on_output_transcript:
                        delta = event.get("delta", "")
                        if not self._print_input_transcript:
                            self._output_transcript_buffer += delta
                        else:
                            if self._output_transcript_buffer:
                                await asyncio.to_thread(
                                    self.on_output_transcript,
                                    self._output_transcript_buffer,
                                )
                                self._output_transcript_buffer = ""
                            await asyncio.to_thread(self.on_output_transcript, delta)
                elif event_type == "response.audio_transcript.done":
                    self._print_input_transcript = False
                elif event_type in self.extra_event_handlers:
                    self.extra_event_handlers[event_type](event)

        except websockets.exceptions.ConnectionClosed:
            print("连接已关闭")
        except Exception as e:
            print("消息处理错误: ", str(e))

    async def close(self) -> None:
        """Close the WebSocket connection."""
        if self.ws:
            await self.ws.close()