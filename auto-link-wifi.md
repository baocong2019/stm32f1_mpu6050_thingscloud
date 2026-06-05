# ESP8266 WiFi 自动连接 AT 指令流程

## 整体策略

设备上电后按以下逻辑自动连接 WiFi：

```
┌──────────────────────────────────────────┐
│          设备上电 / 重启                    │
└──────────────────┬───────────────────────┘
                   ▼
┌──────────────────────────────────────────┐
│  Phase 1: 尝试已保存的 WiFi (20s 超时)      │
│  AT+CWMODE=1                             │
│  AT+CWAUTOCONN=1                         │
│  等待 "WIFI CONNECTED" URC               │
└──────────────────┬───────────────────────┘
         ┌─────────┴─────────┐
         ▼                   ▼
    连接成功              20s 超时
         │                   │
         ▼                   ▼
   ┌──────────┐   ┌──────────────────────────────────┐
   │ 直接使用  │   │  Phase 2: SmartConfig 配网 (120s)  │
   │ 已保存WiFi│   │  AT+CWMODE=1                      │
   └──────────┘   │  AT+CWAUTOCONN=1                  │
                  │  AT+CWSTARTSMART=3                │
                  │  等待 "WIFI CONNECTED" URC        │
                  │  AT+CWSTOPSMART                   │
                  └──────────────────────────────────┘
```

## Phase 1: 自动连接已保存的 WiFi（正常情况）

适用于：之前已通过 SmartConfig 配网成功，WiFi 凭证已保存在 ESP8266 内部 Flash。

```
AT+CWMODE=1          # 设置 Station 模式
AT+CWAUTOCONN=1      # 开启自动连接（ESP8266 上电自动连接已保存的 AP）
```

然后被动等待 ESP8266 的 URC 消息 `WIFI CONNECTED`，超时时间 **20 秒**。

- 如果 AP 在线且信号正常，通常 **3~10 秒** 内即可连接成功
- 如果 20 秒内未收到 `WIFI CONNECTED`，进入 Phase 2

**注意：此阶段不需要也不应该调用 `AT+RESTORE`**，否则会清除已保存的 WiFi 凭证。

## Phase 2: SmartConfig 配网（WiFi 不可用时）

适用于：首次使用（无已保存 WiFi）、路由器更换、WiFi 密码变更等场景。

```
AT+CWMODE=1          # 设置 Station 模式
AT+CWAUTOCONN=1      # 开启自动连接（配网成功后下次自动连接）
AT+CWSTARTSMART=3    # 启动 SmartConfig（3 = ESP-TOUCH + AirKiss）
```

### 配网步骤（用户操作）：

1. OLED 屏幕显示 "SmartConfig: waiting..."
2. 手机连接目标 WiFi（2.4GHz，SmartConfig 不支持 5GHz）
3. 打开 **ESP-TOUCH** App（Espressif 官方 App）或 **AirKiss** 兼容 App
4. 输入 WiFi 密码，点击"配网"
5. ESP8266 收到凭证后自动连接并保存
6. 设备收到 `WIFI CONNECTED` URC
7. 固件发送 `AT+CWSTOPSMART` 停止 SmartConfig

### 重要说明：

- **不要先调用 `AT+RESTORE`**：SmartConfig 成功后 ESP8266 会自动保存新的 WiFi 凭证，下次启动 Phase 1 即可直接连接
- SmartConfig 超时时间 **120 秒**（给用户足够的操作时间）
- SmartConfig 成功或超时后都必须发送 `AT+CWSTOPSMART` 以退出配网模式

## 调试输出

所有 AT 命令和响应都会通过 USART2（115200 bps）输出，格式如下：

```
[CMD] AT+CWMODE=1

OK
[CMD] AT+CWAUTOCONN=1

OK
[DEBUG] Waiting for WIFI CONNECTED...
WIFI CONNECTED
[DEBUG] WIFI CONNECTED detected!
```

## OLED 状态显示

| 显示内容 | 含义 |
|---------|------|
| `W:SV` | 正在尝试已保存的 WiFi |
| `W:SC` | 正在 SmartConfig 配网 |
| `W:OK` | WiFi 已连接 |
| `W:FAIL` | WiFi 连接失败 |
| `W:--` | 空闲/未初始化 |
| `M:OK` | MQTT 已连接 |
| `M:--` | MQTT 未连接 |

## SmartConfig 手机 App

- **ESP-TOUCH**（推荐）：Espressif 官方 App，iOS App Store / Android 应用市场可下载
- **AirKiss**：微信 AirKiss 协议兼容的 App（如微信公众号配网）

## 注意事项

1. **只能配网 2.4GHz WiFi**，不支持 5GHz
2. 手机必须连接目标 WiFi 后再进行 SmartConfig
3. 如果配网失败，检查 ESP8266 固件是否支持 SmartConfig（AT 固件 v1.7+ 默认支持）
4. `AT+CWSTARTSMART=3` 中的 `3` 表示同时启用 ESP-TOUCH 和 AirKiss 两种协议
