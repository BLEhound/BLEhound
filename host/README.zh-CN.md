> [English](README.md) | **中文**

# BLEhound 主机工具

一组 Python 工具，把嗅探器的 USB 数据流转换成 Wireshark 抓包。

- `nrf_sniffer_extcap.py` —— Wireshark 的 [extcap](https://www.wireshark.org/docs/man-pages/extcap.html)
  插件。Wireshark 调用它来列出接口/选项并串流数据包。它会打开嗅探器的 USB 串口，
  解帧（COBS），并把 PCAP（`LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR`）写入 Wireshark 的
  fifo。
- `tri_aggregator.py` —— 纯逻辑模块，把三块板做时间对齐并合并成单一数据流
  （`SyncClock` + `Aggregator` + `FollowRelay`）。可运行自测：
  `python3 tri_aggregator.py --selftest`。
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
cp nrf_sniffer_extcap.py tri_aggregator.py ~/.local/lib/wireshark/extcap/
chmod +x ~/.local/lib/wireshark/extcap/nrf_sniffer_extcap.py
```

重启 Wireshark。接口列表中会出现 **nRF BLE Sniffer**（单板）和
**nRF BLE Sniffer (3ch aggregated)**(三板合并)。

固件对外声明的 USB 标识：VID `0x1915`、PID `0x520F`，产品字符串
`nRF BLE Sniffer`。

## 单板 vs 三板

- **单板：** 选择 `nRF BLE Sniffer` 接口。在接口选项里设置扫描信道或目标 MAC。
- **三板：** 选择 **nRF BLE Sniffer (3ch aggregated)** 接口（可选启用三板接力）。每块板把守
  37 / 38 / 39；聚合器会合并并去重成单一时间线。SYNC 线的接线方式见文档。

过滤器书签：加载 `wireshark_dfilters` 中的条目，可快速隔离出目标设备、连接数据、
CONNECT_IND、CRC 错误等。把示例地址 `aa:bb:cc:dd:ee:ff` 替换为你设备的地址。
