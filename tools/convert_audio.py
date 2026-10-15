#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
音频文件转换工具
将 MP3 音频文件转换为 ESP32 可用的 C 头文件格式

使用方法:
    python convert_audio.py

功能:
    - 自动扫描 ../assets/mock_voices/ 目录中的 MP3 文件
    - 使用 ffmpeg 将 MP3 转换为 16kHz 单声道 16位 PCM 格式
    - 生成对应的 C 头文件到 ../components/voice_assets/include/mock_voices/
    - 支持覆盖已存在的头文件

依赖:
    - ffmpeg (需要在系统 PATH 中)
    - python3

作者: Augment Agent
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
    使用 ffmpeg 将 MP3 转换为 16kHz 单声道 16位 PCM 格式
    
    Args:
        mp3_path: 输入的 MP3 文件路径
        output_path: 输出的 PCM 文件路径
    
    Returns:
        bool: 转换是否成功
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
            print(f"❌ ffmpeg 转换失败: {result.stderr}")
            return False
            
        return True
        
    except Exception as e:
        print(f"❌ 转换过程中发生错误: {e}")
        return False


def pcm_to_c_header(pcm_path, header_path, array_name):
    """
    将 PCM 文件转换为 C 头文件格式
    
    Args:
        pcm_path: PCM 文件路径
        header_path: 输出的头文件路径
        array_name: C 数组名称
    
    Returns:
        bool: 转换是否成功
    """
    try:
        # 读取 PCM 数据
        with open(pcm_path, 'rb') as f:
            pcm_data = f.read()
        
        if len(pcm_data) == 0:
            print(f"❌ PCM 文件为空: {pcm_path}")
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
        
        print(f"✓ 生成头文件: {header_path} (数组大小: {len(pcm_data)} 字节)")
        return True
        
    except Exception as e:
        print(f"❌ 生成头文件失败: {e}")
        return False


def convert_audio_file(mp3_path, output_dir):
    """
    转换单个音频文件
    
    Args:
        mp3_path: MP3 文件路径
        output_dir: 输出目录
    
    Returns:
        bool: 转换是否成功
    """
    mp3_path = Path(mp3_path)
    output_dir = Path(output_dir)
    
    # 生成输出文件名
    base_name = mp3_path.stem  # 不包含扩展名的文件名
    header_path = output_dir / f"{base_name}.h"
    array_name = base_name  # 使用文件名作为数组名
    
    print(f"🔄 正在转换: {mp3_path.name}")
    
    # 创建临时 PCM 文件
    with tempfile.NamedTemporaryFile(suffix='.pcm', delete=False) as temp_pcm:
        temp_pcm_path = temp_pcm.name
    
    try:
        # 第一步：MP3 转 PCM
        if not convert_mp3_to_pcm(mp3_path, temp_pcm_path):
            return False
        
        # 第二步：PCM 转 C 头文件
        if not pcm_to_c_header(temp_pcm_path, header_path, array_name):
            return False
        
        print(f"✅ 转换完成: {mp3_path.name} -> {header_path.name}")
        return True
        
    finally:
        # 清理临时文件
        try:
            os.unlink(temp_pcm_path)
        except:
            pass


def main():
    """主函数"""
    print("🎵 ESP32 音频文件转换工具")
    print("=" * 50)
    
    # 检查 ffmpeg
    if not check_ffmpeg():
        print("❌ 错误: 未找到 ffmpeg")
        print("请确保 ffmpeg 已安装并在系统 PATH 中")
        print("macOS: brew install ffmpeg")
        print("Ubuntu: sudo apt install ffmpeg")
        sys.exit(1)
    
    print("✓ ffmpeg 检查通过")
    
    # 确定路径：
    #   MP3 源文件   -> assets/mock_voices/
    #   生成的头文件 -> components/voice_assets/include/mock_voices/
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    mp3_dir = project_root / "assets" / "mock_voices"
    header_dir = project_root / "components" / "voice_assets" / "include" / "mock_voices"

    if not mp3_dir.exists():
        print(f"❌ 错误: 未找到目录 {mp3_dir}")
        sys.exit(1)

    header_dir.mkdir(parents=True, exist_ok=True)

    print(f"📁 扫描目录: {mp3_dir}")
    print(f"📁 输出目录: {header_dir}")

    # 查找所有 MP3 文件
    mp3_files = list(mp3_dir.glob("*.mp3"))
    
    if not mp3_files:
        print("⚠️  未找到任何 MP3 文件")
        return
    
    print(f"📄 找到 {len(mp3_files)} 个 MP3 文件:")
    for mp3_file in mp3_files:
        print(f"  - {mp3_file.name}")
    
    print("\n🔄 开始转换...")
    
    # 转换每个文件
    success_count = 0
    for mp3_file in mp3_files:
        if convert_audio_file(mp3_file, header_dir):
            success_count += 1
        print()  # 空行分隔
    
    # 输出结果
    print("=" * 50)
    print(f"✅ 转换完成: {success_count}/{len(mp3_files)} 个文件成功")
    
    if success_count > 0:
        print("\n📝 生成的头文件:")
        for mp3_file in mp3_files:
            header_file = header_dir / f"{mp3_file.stem}.h"
            if header_file.exists():
                print(f"  - {header_file.name}")
        
        print("\n💡 使用提示:")
        print("1. 在 C 代码中包含头文件: #include \"mock_voices/filename.h\"")
        print("2. 使用数组: bsp_play_audio(array_name, array_name_len);")


if __name__ == "__main__":
    main()
