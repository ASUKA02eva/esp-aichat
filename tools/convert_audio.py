#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
音频素材转换：把 assets/mock_voices/*.mp3 转成 16kHz/单声道/16bit 的 C 头文件，
输出到 components/voice_assets/include/mock_voices/。

每个头文件导出 <文件名>[]（unsigned char 数组）与 <文件名>_len（数组长度，单位字节）；
已存在的头文件会被覆盖。
依赖：系统 PATH 中需有 ffmpeg。
"""

import os
import sys
import subprocess
import tempfile
from pathlib import Path


def check_ffmpeg():
    """检查 ffmpeg 是否可用"""
    try:
        subprocess.run(['ffmpeg', '-version'], 
                      stdout=subprocess.DEVNULL, 
                      stderr=subprocess.DEVNULL, 
                      check=True)
        return True
    except (subprocess.CalledProcessError, FileNotFoundError):
        return False


def convert_mp3_to_pcm(mp3_path, output_path):
    """
    调用 ffmpeg 转成 16kHz 单声道 16bit little-endian PCM，失败返回 False。

    转换失败时 stderr 内容会打印到控制台。
    """
    try:
        cmd = [
            'ffmpeg',
            '-i', str(mp3_path),           # 输入文件
            '-ar', '16000',                # 采样率 16kHz
            '-ac', '1',                    # 单声道
            '-f', 's16le',                 # 16位小端格式
            '-y',                          # 覆盖输出文件
            str(output_path)               # 输出文件
        ]
        
        result = subprocess.run(cmd, 
                              stdout=subprocess.DEVNULL, 
                              stderr=subprocess.PIPE, 
                              text=True)
        
        if result.returncode != 0:
            print(f"ffmpeg 转换失败: {result.stderr}")
            return False
            
        return True
        
    except Exception as e:
        print(f"转换过程中发生错误: {e}")
        return False


def pcm_to_c_header(pcm_path, header_path, array_name):
    """
    把 PCM 字节写成 C 头文件，每行 16 字节十六进制。

    array_name 即数组名，长度符号为 <array_name>_len。PCM 为空或写入失败返回 False。
    """
    try:
        # 读取 PCM 数据
        with open(pcm_path, 'rb') as f:
            pcm_data = f.read()
        
        if len(pcm_data) == 0:
            print(f"PCM 文件为空: {pcm_path}")
            return False
        
        # 生成 C 头文件内容
        header_content = f"""#include <stdio.h>
const unsigned char {array_name}[] = {{
"""
        
        # 将字节数据转换为十六进制格式，每行16个字节
        for i in range(0, len(pcm_data), 16):
            line_data = pcm_data[i:i+16]
            hex_values = [f"0x{byte:02x}" for byte in line_data]
            header_content += ", ".join(hex_values) + ", \n"
        
        # 移除最后的逗号和换行，添加结束括号
        header_content = header_content.rstrip(", \n") + " \n"
        header_content += "};\n"
        header_content += f"const unsigned int {array_name}_len = {len(pcm_data)};\n"
        
        # 写入头文件
        with open(header_path, 'w', encoding='utf-8') as f:
            f.write(header_content)
        
        print(f"生成头文件: {header_path} (数组大小: {len(pcm_data)} 字节)")
        return True
        
    except Exception as e:
        print(f"生成头文件失败: {e}")
        return False


def convert_audio_file(mp3_path, output_dir):
    """
    转换单个 MP3：经临时 PCM 中转后生成头文件，无论成败都会删除临时文件。

    返回是否转换成功。
    """
    mp3_path = Path(mp3_path)
    output_dir = Path(output_dir)
    
    # 生成输出文件名
    base_name = mp3_path.stem  # 不包含扩展名的文件名
    header_path = output_dir / f"{base_name}.h"
    array_name = base_name  # 使用文件名作为数组名
    
    print(f"正在转换: {mp3_path.name}")
    
    # 创建临时 PCM 文件
    with tempfile.NamedTemporaryFile(suffix='.pcm', delete=False) as temp_pcm:
        temp_pcm_path = temp_pcm.name
    
    try:
        if not convert_mp3_to_pcm(mp3_path, temp_pcm_path):
            return False
        
        if not pcm_to_c_header(temp_pcm_path, header_path, array_name):
            return False
        
        print(f"转换完成: {mp3_path.name} -> {header_path.name}")
        return True
        
    finally:
        # 清理临时文件
        try:
            os.unlink(temp_pcm_path)
        except:
            pass


def main():
    """入口：检查 ffmpeg 后批量转换 mock_voices 下的 MP3"""
    print("ESP32 音频文件转换工具")
    print("=" * 50)
    
    # 检查 ffmpeg
    if not check_ffmpeg():
        print("错误: 未找到 ffmpeg")
        print("请确保 ffmpeg 已安装并在系统 PATH 中")
        print("macOS: brew install ffmpeg")
        print("Ubuntu: sudo apt install ffmpeg")
        sys.exit(1)
    
    print("ffmpeg 检查通过")
    
    # 确定路径：
    #   MP3 源文件   -> assets/mock_voices/
    #   生成的头文件 -> components/voice_assets/include/mock_voices/
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    mp3_dir = project_root / "assets" / "mock_voices"
    header_dir = project_root / "components" / "voice_assets" / "include" / "mock_voices"

    if not mp3_dir.exists():
        print(f"错误: 未找到目录 {mp3_dir}")
        sys.exit(1)

    header_dir.mkdir(parents=True, exist_ok=True)

    print(f"扫描目录: {mp3_dir}")
    print(f"输出目录: {header_dir}")

    # 查找所有 MP3 文件
    mp3_files = list(mp3_dir.glob("*.mp3"))
    
    if not mp3_files:
        print("未找到任何 MP3 文件")
        return
    
    print(f"找到 {len(mp3_files)} 个 MP3 文件:")
    for mp3_file in mp3_files:
        print(f"  - {mp3_file.name}")
    
    print("\n开始转换...")
    
    # 转换每个文件
    success_count = 0
    for mp3_file in mp3_files:
        if convert_audio_file(mp3_file, header_dir):
            success_count += 1
        print()  # 空行分隔
    
    # 输出结果
    print("=" * 50)
    print(f"转换完成: {success_count}/{len(mp3_files)} 个文件成功")
    
    if success_count > 0:
        print("\n生成的头文件:")
        for mp3_file in mp3_files:
            header_file = header_dir / f"{mp3_file.stem}.h"
            if header_file.exists():
                print(f"  - {header_file.name}")
        
        print("\n使用提示:")
        print("1. 在 C 代码中包含头文件: #include \"mock_voices/filename.h\"")
        print("2. 使用数组: bsp_play_audio(array_name, array_name_len);")


if __name__ == "__main__":
    main()
