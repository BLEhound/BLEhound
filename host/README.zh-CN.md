> [English](README.md) | **中文**

# BLEhound 主机工具

一组 Python 工具，把嗅探器的 USB 数据流转换成 Wireshark 抓包。

- `blehound_extcap.py` —— Wireshark 的 [extcap](https://www.wireshark.org/docs/man-pages/extcap.html)
  插件。Wireshark 调用它来列出接口/选项并串流数据包。它会打开嗅探器的 USB 串口，
  解帧（COBS），并把 PCAP（`LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`）写入 Wireshark 的
  fifo。
- `blehound_extcap.bat` —— Windows 包装器:Windows 上 Wireshark 只跑 `.exe`/`.bat`,
  不直接跑 `.py`,所以由它用 Python 启动插件。
- `blehound_tri_aggregator.py` —— 纯逻辑模块，把三块板做时间对齐并合并成单一数据流
  （`SyncClock` + `Aggregator` + `FollowRelay`）。可运行自测：
  `python3 blehound_tri_aggregator.py --selftest`。
- `wireshark_dfilters` —— 一组好用的 Wireshark 显示过滤器书签。
- `security/` —— 传统配对（legacy-pairing）分析与解密辅助工具（见其 README）。

## 环境要求

Python 3.9+（Wireshark 使用系统的 `python3`；在 macOS 上即 3.9）。

```bash
pip install -r requirements.txt      # pyserial (+ cryptography for security/)
```

## 安装到 Wireshark

```bash
../tools/install_extcap.sh           # copies the extcap into Wireshark's extcap dir
```

或手动安装：

```bash
# Wireshark 4.x 路径(旧版 <4:~/.config/wireshark/extcap)
mkdir -p ~/.local/lib/wireshark/extcap
cp blehound_extcap.py blehound_tri_aggregator.py ~/.local/lib/wireshark/extcap/
chmod +x ~/.local/lib/wireshark/extcap/blehound_extcap.py
```

重启 Wireshark。接口列表中会出现 **BLEhound Sniffer**（单板）和
**BLEhound Sniffer (3ch aggregated)**(三板合并)。

### Windows

Windows 上 Wireshark 只执行 `.exe` / `.bat` 形式的 extcap,不直接跑 `.py`,所以必须用随附的 `.bat` 包装器:

1. 装 Python 3(勾选 **Add python.exe to PATH**),执行 `pip install pyserial`。
2. 在 Wireshark 里找 extcap 目录:**帮助 → 关于 Wireshark → 文件夹 → Personal Extcap path**(通常是 `%APPDATA%\Wireshark\extcap`)。
3. 把 **`blehound_extcap.py`、`blehound_extcap.bat`、`blehound_tri_aggregator.py`** 三个文件复制进该目录,重启 Wireshark。

嗅探器的串口在 Windows 上显示为 `COMx`,其余用法完全相同。若 `python` 不在 PATH,把 `.bat` 里的 `python` 改成 `py -3`。`tools/*.sh` 脚本仅限 macOS/Linux(Windows 上想用可走 Git Bash / WSL)。

固件对外声明的 USB 标识：VID `0x1915`、PID `0x520F`，产品字符串
`BLEhound Sniffer`。

## 单板 vs 三板

- **单板：** 选择 `BLEhound Sniffer` 接口。在接口选项里设置扫描信道或目标 MAC。
- **三板：** 选择 **BLEhound Sniffer (3ch aggregated)** 接口（可选启用三板接力）。每块板把守
  37 / 38 / 39；聚合器会合并并去重成单一时间线。SYNC 线的接线方式见文档。

过滤器书签：加载 `wireshark_dfilters` 中的条目，可快速隔离出目标设备、连接数据、
CONNECT_IND、CRC 错误等。把示例地址 `aa:bb:cc:dd:ee:ff` 替换为你设备的地址。
