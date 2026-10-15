# GitHub Actions 工作流说明

## build.yml

这个工作流用于自动构建验证整个固件（合并后只有一个 ESP-IDF 工程，不再是多个示例）。

### 触发条件

- 有 Pull Request 提交到 `main` 分支时
- 推送到 `main` 分支且改动不只在 `docs/`、`server/`、`.md` 文件里时
- 手动触发（通过 GitHub Actions 界面）

### 构建配置

- 目标芯片：`esp32s3`
- ESP-IDF 版本：`v5.4`
- 构建入口：仓库根目录（`idf.py build`）

### 本地测试

可以用官方 Docker 镜像在本地复现：

```bash
# Linux / macOS
docker run --rm -v $PWD:/project -w /project -u $(id -u):$(id -g) -e HOME=/tmp \
  espressif/idf:release-v5.4 idf.py set-target esp32s3 build

# Windows (PowerShell)
docker run --rm -v ${PWD}:/project -w /project \
  espressif/idf:release-v5.4 idf.py set-target esp32s3 build
```
