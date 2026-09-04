# FlashLog — 具斷電復原能力的 SPI NOR Flash 日誌韌體
 
在 STM32F446 + FreeRTOS + SPI NOR Flash 上實作的 append-only log storage，
具備 CRC32 完整性驗證、開機自動復原、環形空間回收與磨損統計，
並附 Python host CLI 與效能量測工具。
 
---
 
## 這個專案解決什麼問題
 
SPI NOR Flash 有三個物理限制，使它不能當成一般記憶體使用：
 
- **不能原地覆寫** —— 寫入只能把 bit 從 1 變 0，要變回 1 只能擦除
- **擦除的最小單位是 4 KB sector** —— 想改一個 byte，得擦掉整個 sector
- **寫入中斷電會留下半筆資料** —— 而且它「看起來」可能完全合法
FlashLog 在這些限制下，實作一個**可持續運轉、且能從斷電中自我修復**的日誌儲存層。
 
---
 
## 特色
 
- **Append-only log** —— record 不跨越 sector 邊界，空間不足時寫入 PAD record
- **CRC32 完整性驗證 + 開機自動復原** —— 可用內建的故障注入指令重現與驗證
- **環形空間回收** —— 寫滿後回收最舊 sector，實測 16 個 sector 磨損 spread ≤ 2
- **FreeRTOS 雙 task 架構** —— queue 分派請求，flash 採 single-owner 設計（無需鎖）
- **UART 指令介面** —— 中斷驅動接收，`OK` / `ERR <code>` 結束標記協定
- **Python host CLI** —— 自動化測試、benchmark、CSV 輸出、圖表產生
- **DWT 微秒級量測** —— append 0.46 ms、sector erase 55.4 ms、mount 成本模型
---
 
## 系統架構
 
### 模組分層
 
```
┌──────────────────────────────────────────────┐
│  main.c        初始化 + 兩個 FreeRTOS task   │
├───────────────┬──────────────────────────────┤
│  cmd.c        │  指令解析（文字 → 請求結構） │
│  storage.h/.c │  task 間訊息契約 + queue      │
├───────────────┴──────────────────────────────┤
│  log.c         append-only log 語意          │
│                record / write pointer / GC   │
├───────────────┬──────────────────────────────┤
│  crc32.c      │  wear.c    磨損統計          │
│  完整性驗證   │  （append-only 事件）        │
├───────────────┴──────────────────────────────┤
│  flash.c       SPI NOR 裝置驅動              │
│                erase / program / read        │
├──────────────────────────────────────────────┤
│  log_uart.c    UART 輸出與中斷接收           │
│  perf.c        DWT 微秒計時                  │
└──────────────────────────────────────────────┘
```
 
每一層只依賴下一層：`log.c` 不知道 SPI 存在，`flash.c` 不知道上層在存什麼。
 
### 執行時架構
 
```
  PuTTY / Python CLI
        │ UART
        ▼
  ┌───────────┐   rx queue    ┌──────────────┐
  │ USART2 IRQ├──────────────►│ CommandTask  │
  └───────────┘  (ISR 只做    │  解析指令     │
                  最小的事)    └──────┬───────┘
                                      │ storage queue
                                      ▼
                              ┌──────────────┐
                              │ StorageTask  │──► flash driver
                              │  依序處理     │    (single owner)
                              └──────┬───────┘
                                     │
        UART mutex 保護共享輸出  ◄────┘
```
 
**兩個並行設計要點：**
 
- **flash 不需要鎖** —— 只有 StorageTask 碰得到它（single-owner pattern）。
  沒有鎖就沒有死鎖、沒有優先權反轉。
- **UART 需要鎖** —— 兩個 task 都要輸出，而 `printf` 非 thread-safe。
  115200 baud 下一行 30 字約 2.6 ms，遠超過 1 ms 的 RTOS tick，中間隨時可能被切走。
---
 
## Flash Layout
 
```
0x000000  ┌────────────────────────┐
          │  metadata sector       │  磨損統計（append-only 事件，每筆 4 B）
0x001000  ├────────────────────────┤
          │  S0   S1   S2  ...  S15│  log 資料區：16 × 4 KB sector
          │                        │  環形使用，寫滿回收最舊
0x011000  └────────────────────────┘
```
 
### 環形回收
 
```
      寫入方向 ──►
  ┌────┬────┬────┬────┬────┬────┐
  │ S0 │ S1 │ S2 │ S3 │ .. │S15 │
  └────┴────┴────┴────┴────┴────┘
    ▲         ▲
    │         └── active（目前寫入中）
    └── oldest（下次回收目標）
 
  當 active 的下一個 == oldest，代表繞了一圈 → 擦除 oldest
```
 
`oldest` 與 `active` **不儲存在 flash 中**，而是 mount 時掃描各 sector 首筆 record 的
`rec_id` 推導出來（最小者為 oldest、最大者為 active）。
 
### Record 格式
 
```
┌─────────── header (20 bytes, packed) ────────┬── payload ──┐
│ magic │ rec_id │ len │ rsvd │ ts  │  crc32   │   資料...    │
│  4 B  │  4 B   │ 2 B │ 2 B  │ 4 B │   4 B    │  len bytes  │
└──────────────────────────────────────────────┴─────────────┘
```
 
| 欄位 | 為什麼存在 |
|---|---|
| `magic` | 掃描時判斷「這裡是否有一筆 record」。抹除後的 flash 是全 `FF`，而 magic 不可能是 `0xFFFFFFFF`——**因此第一個讀到 FF 的位置就是 log 尾端** |
| `rec_id` | 不隨位址改變的穩定識別；同時作為 sector 的「年紀」用於環形 mount |
| `length` | 知道 payload 多長才算得出下一筆位置——**遍歷的依據** |
| `reserved` | 對齊；保留給未來的 flags |
| `timestamp` | `HAL_GetTick()`（已知限制：跨重開機歸零） |
| `crc32` | 偵測「header 合法但 payload 不完整」的斷電殘骸 |
 
**兩個設計細節：**
 
- **`crc32` 放在 header 最後** —— 因為 CRC 不能涵蓋自己，放最後讓計算範圍是乾淨的
  「前 16 bytes」，不必在中間挖洞。
- **`__attribute__((packed))`** —— 防止編譯器 padding 造成寫入／讀取錯位。
  **任何要寫進儲存裝置的結構都必須 packed。**
### PAD record
 
record 不跨 sector，空間不足時寫入一筆 `magic = "PAD!"` 的 header，
宣告「本 sector 剩下的不用了」。若剩餘空間連 header 都放不下，則留全 `FF`，
由掃描端的邊界判斷處理。
 
掃描邏輯是四個分支，**窮舉 flash 上所有可能狀態**：
 
| 狀態 | 動作 |
|---|---|
| 剩餘空間 < header | sector 尾端碎片 → 跳到下個 sector |
| `magic == 0xFFFFFFFF` | 從沒寫過 → log 到此為止 |
| `magic == "PAD!"` | padding → 跳到下個 sector |
| `magic == "FLRG"` | 正常 record → 驗證 CRC |
 
---
 
## 斷電復原
 
寫入中斷電會留下三種殘骸，系統必須全部認得：
 
| 情況 | flash 上的樣子 | 偵測方式 |
|---|---|---|
| **A** header 沒寫完 | magic 是部分 FF 或亂碼 | `magic != LOG_MAGIC` |
| **B** header 完整、payload 只寫一半 | **看起來完全合法**，但 payload 尾端是 FF | **只有 CRC 抓得到** |
| **C** 完整寫入 | 正常 | CRC 通過 |
 
**情況 B 是 CRC 存在的唯一理由。** 沒有 CRC，系統會信任那筆半殘的 record，
從**錯誤的位置**繼續 append——之後所有資料錯位，**整個 log 結構崩壞**，
而不只是少一筆。
 
### 故障注入
 
為了讓復原機制**可測試、可重現**（而不是靠運氣拔電線），內建兩個指令：
 
```
log corrupt          寫一筆 CRC 錯誤的 record
log partial <text>   只寫 header + 一半 payload（模擬寫入中斷電）
```
 
`log partial` 寫出的 record **在各方面都合法**——magic 正確、length 正確、
CRC 也是完整資料算出的正確值。唯一的問題是 payload 只有一半。
**這正是真實斷電會產生的狀態，也是最危險的一種。**
 
實測結果：
 
```
FlashLog> log partial abcdefghij
[LOG ] injected partial record @0x001017 (5 of 10 bytes)
→ RESET
[LOG ] corrupt record @0x001017, truncating here
[LOG ] init: 1 records, wp=0x001017, next_id=2 (recovered)
```
 
write pointer 精準退回損毀那筆的起點，半殘的 record 被視為不存在。
 
---
 
## 效能與量測
 
所有數據由韌體端的 **DWT cycle counter**（解析度 62.5 ns）量測，
經 host CLI 收集為 CSV，再由 `tools/plot_*.py` 產圖。**全流程可重現。**
 
### Mount 成本模型與優化歷程
 
![mount scaling](docs/mount_scaling.png)
 
Mount 時間隨 record 數**線性成長（O(n)）**——三組 payload 尺寸、8 個測量點，
線性度誤差 < 1%。
 
**成本模型：** `t_mount(n, s) ≈ n × (734 + 9.7 × s) µs`　（n = 筆數，s = payload bytes）
 
**兩輪優化：**
 
| 版本 | 固定成本 | 變動成本 |
|---|---|---|
| v1　逐位元 CRC32、SPI 1 MHz | 1361 µs | 27.3 µs/B |
| v2　**查表 CRC32**、SPI 1 MHz | 1165 µs | 17.7 µs/B |
| v3　查表 CRC32、**SPI 8 MHz** | **734 µs** | **9.7 µs/B** |
 
**變動成本的拆解**（面板 c）：CRC 演算法 35%、SPI 傳輸 29%、HAL per-byte 開銷 36%。
 
其中 **SPI 傳輸的 8.0 µs/B 與 1 MHz 下的理論值完全吻合**（每 byte 8 個時脈週期），
交叉驗證了這個拆解的正確性。
 
> **一個反直覺的發現：** 在 8 MHz 下，實際傳輸一個 byte 只要 1 µs，
> 但剩餘的 9.7 µs/B 推測來自 HAL 的 per-byte 開銷（狀態檢查、timeout 處理、
> 迴圈控制）——**抽象層的成本是實際 I/O 的 10 倍。**
 
### GC 造成的 tail latency
 
![gc latency](docs/gc_latency.png)
 
在 log 已寫滿的狀態下量測 1000 次 append（payload 32 B）：
 
| | 延遲 |
|---|---|
| p50 | 8.82 ms |
| p95 | 9.35 ms |
| **p99** | **62.29 ms** |
| max | 65.70 ms |
| mean | 9.34 ms |
 
**p95 → p99 跳升 6.7 倍。**
 
1000 筆中出現 10 次尖峰，**間隔精確為 78 次 append**，無一例外。
驗算：record = 20 (header) + 32 (payload) = 52 B，`4096 / 52 = 78.8`
→ 每個 4 KB sector 恰好容納 78 筆。**尖峰不是隨機的，由 sector 容量決定。**
 
擦除成本 = 尖峰平均 − p50 = **54.9 ms**，
與單獨量測的 `last_erase_us = 55383`（55.4 ms）**相差 0.5%**。
 
> **注意 `mean` (9.34 ms) 幾乎等於 `p50` (8.82 ms)，完全掩蓋了長尾。**
> 這是只看平均值會誤判的典型例子。
 
### 磨損分布
 
連續寫入 4000 筆（payload 44 B）後：
 
```
S0–S3  : 5 次        total = 56
S4–S15 : 3 次        spread = 2
```
 
環形回收使擦除**嚴格輪序**：每繞一圈所有 sector 各擦一次，餘數依序落在前幾個。
**因此單次連續運轉下，spread 必然 ≤ 1。**
 
本次 spread = 2 來自**兩輪測試的累積**——每次 `log format` 後寫入都從 S0 重新開始，
使餘數重複落在同一批 sector 上。詳見〈已知限制〉。
 
---
 
## 快速開始
 
### 硬體
 
| 項目 | 型號 |
|---|---|
| MCU 板 | STM32 Nucleo-F446RE |
| Flash | SPI NOR，實測為 XTX **XT25F128B**（16 MB，JEDEC ID `0B 40 18`）※ |
 
※ 模組標示為 W25Q 系列，但讀取 JEDEC ID 後確認實際晶片為 XTX 相容品。
命令集、page/sector 大小與 W25Q 相同。**這也是 bring-up 第一步要讀 ID 驗證的理由。**
 
### 接線
 
以 Arduino 排針的絲印為準，不需查腳位表：
 
| Flash 腳 | 意義 | Nucleo |
|---|---|---|
| VCC | 供電 | **3V3** |
| GND | 接地 | GND |
| CS | Chip Select | `PWM/CS/D10`（PB6） |
| **DI** | flash 輸入 = MOSI | `PWM/MOSI/D11`（PA7） |
| **DO** | flash 輸出 = MISO | `MISO/D12`（PA6） |
| CLK | 時脈 | `SCK/D13`（PA5） |
 
 **`DO` 接 MISO、`DI` 接 MOSI** —— 是「輸出接輸入」的交叉對應，
 
### 建置
 
以 STM32CubeMX 開啟 `FlashLog_MX.ioc` 產生程式碼，再用 STM32CubeIDE 建置與燒錄。
 
> 註：自 STM32CubeIDE V2.0.0 起，CubeMX 已從 IDE 中分離，需另外安裝獨立版 CubeMX。
 
### 使用
 
序列埠設定 **115200 8-N-1**：
 
```
FlashLog> help
  id | erase <addr> | write <addr> <text> | read <addr> <len>
  log write <text>       | log read <id>
  log dump [start] [cnt] | log stats
  log format | log remount
  log wear   | log wearreset
  log corrupt | log partial <text>
```
 
### Host CLI
 
```bash
pip install pyserial
cd tools
 
python flashlog.py --port COM3 format
python flashlog.py write hello
python flashlog.py dump
python flashlog.py stats
python flashlog.py wear
 
# 量測
python flashlog.py benchmark --count 1000 --size 32 --csv data/latency.csv
python flashlog.py scaling   --max 200 --step 25 --csv data/scaling.csv
 
# 產圖
python plot_scaling.py    --indir data --out ../docs/mount_scaling.png
python plot_gc_latency.py --csv data/gc_latency_full.csv --out ../docs/gc_latency.png
```
 
**Host-device 協定：** 每個指令的回應**一律以 `OK` 或 `ERR <code>` 結尾**。
這讓工具能明確知道指令何時完成——不同指令耗時相差兩個數量級（`id` 約 1 ms、
`erase` 約 55 ms），若靠固定等待時間會不是漏讀就是浪費。
 
---
 
## 設計決策
 
<details>
<summary><b>為什麼 record 不跨越 sector 邊界？</b></summary>
<br>
引入 GC 後，sector 成為回收的單位。若一筆 record 跨在兩個 sector 之間，
回收前半時會留下「沒有 header 的後半段」——掃描時會讀到垃圾，
可能被誤判成 magic 或算出離譜的 length。**GC 會破壞自己的 recovery 機制。**
 
其他兩個理由：
- **走訪的獨立性** —— 每個 sector 可獨立解析，能從任意 sector 開始掃、能跳過整個 sector
- **錯誤隔離** —— 單一 sector 損毀不會連累相鄰
**代價：** 每個 sector 尾端有 padding 浪費。最大 record 84 B，
最壞浪費 83 / 4096 ≈ **2%**。
 
**前提是 record 遠小於 sector**（84 : 4096 ≈ 1:49）。
若 record 是 2100 B，一個 4 KB sector 只能放一筆，浪費將近 50%——
**這個設計不適用於大 record。**
 
</details>
<details>
<summary><b>為什麼 metadata 不儲存 write pointer？</b></summary>
<br>
若 write pointer 存在固定位置，每次 append 都要更新它。
但 **flash 不能原地覆寫**，所以每次更新都得「擦除整個 sector → 重寫」。
 
三層問題，一層比一層嚴重：
 
1. **慢** —— append 從 0.5 ms 變成 55 ms
2. **磨損** —— 該 sector 每次 append 擦一次，成為必然先壞的熱點
3. **最致命：更新當下斷電** —— sector 半擦除，metadata 全毀。
   而 metadata 是「用來知道資料在哪」的東西——**它毀了，整個 log 就找不到了，
   即使所有 record 完好無損。**
> 為了保護資料而引入的機制，自己變成了單點故障。
 
**因此所有可推導的狀態都不儲存：**
 
| 不存 | 推導方式 |
|---|---|
| write pointer | mount 掃描到第一個 FF |
| 記錄總數 | 走訪計數 |
| oldest / active sector | 比較各 sector 首筆的 rec_id |
 
**代價是 mount 需要掃描（O(n)）** —— 這正是 log-structured 設計的核心取捨：
**把成本從頻繁的寫入路徑，挪到罕見的開機路徑。**
 
</details>
<details>
<summary><b>為什麼不用背景 GC（像 SSD 和 JVM 那樣）？</b></summary>
<br>
大型系統的 GC 幾乎都是背景執行（JVM concurrent GC、SSD 閒置時的 FTL GC、
Linux 的 kswapd）。本專案採用 **lazy erase（寫滿當下才回收）**，理由是三個前提都不具備：
 
| 條件 | JVM | SSD | 本專案 |
|---|---|---|---|
| 一般操作 : 回收成本 | ~10 ns : 100 ms → **10⁷** | 50 µs : 2 ms → **40** | 0.46 ms : 55 ms → **120** |
| 有閒置資源 |  多核心，真平行 |  裝置大多時間閒置 |  **單核心，flash 為 single-owner** |
| 可中斷 |  incremental GC |  NAND **Erase Suspend** |  driver 未實作 |
| 失敗有備援 |  重啟即可 |  ECC、備援 block |  log 是唯一的持久化狀態 |
 
**關鍵洞察：** 即使開一個背景 GC task，它和 StorageTask 仍搶同一顆 CPU、
同一條 SPI。**「背景」只是換個時間點做，不是不佔資源做。**
沒有 Erase Suspend，55 ms 一旦開始就不可中斷。
 
**正確的解法是實作 Erase Suspend（`0x75`/`0x7A`），而不是單純把擦除搬到背景 task。**
 
系統同時保留 `LOG_MODE_STOP`（寫滿即回報 `FULL`，不丟舊資料）——
logger 與 audit log 的需求不同，主要工作兩種模式共用，差別只在一個判斷。
 
</details>
<details>
<summary><b>為什麼磨損統計用 append-only 事件記錄？</b></summary>
<br>
**循環依賴：** 每擦一次 sector，該 sector 的 count 要 +1。
若存在固定位置就是原地覆寫，flash 做不到；要先擦除，而擦除又要記錄……
 
**解法：用本專案已證明可行的 append-only 模式。**
 
```
metadata sector
┌────┬────┬────┬────┬─────────────┐
│ E:3│ E:5│ E:3│ E:7│ FF FF FF... │
└────┴────┴────┴────┴─────────────┘
  ↑「sector 3 被擦除了」
```
 
要知道 sector 3 擦過幾次，就掃過去數有幾筆 `E:3`；開機時重播所有事件重建計數。
 
**每筆事件僅 4 bytes** —— 內部統計不需要 CRC（丟一筆只是統計少一次，
不影響資料完整性）、不需時間戳、不需 id。最小結構讓一個 sector 能記
**1024 筆事件**。
 
</details>
<details>
<summary><b>為什麼 flash 不需要 mutex，UART 卻需要？</b></summary>
<br>
**判斷要不要加鎖，看的不是「這個資源重不重要」，而是「有幾個執行流會碰它」。**
 
**flash 只有 StorageTask 碰得到** —— 所有請求排進 queue 依序處理，
存取天然序列化。這叫 **single-owner pattern**，比加鎖更好：
沒有鎖就沒有死鎖、沒有優先權反轉，存取順序明確可預測。
 
**UART 兩個 task 都要輸出** —— 而 `printf` 不是 thread-safe。
輸出是一個字元一個字元送的（115200 baud 下每字元約 87 µs），
一行 30 字要 2.6 ms，**遠超過 1 ms 的 RTOS tick**，中間隨時可能被切走，
兩個 task 的字元就會交錯。
 
**附帶好處：** 用 mutex 序列化所有輸出後，同時保證任何時刻只有一個 task
在 printf 內部，連 newlib 的重入問題一起解決了。
 
**但單字元 echo（`uart_putc_raw`）刻意不加鎖** —— 這是取捨：
加鎖會讓打字在 StorageTask 輸出期間被延遲，而交錯只影響顯示美觀。
**即時性優先於顯示整潔，因為真正需要乾淨輸出的是 Python 工具，它不會同時打字。**
 
</details>
---
 
## 已知限制
 
這些是評估後選擇不處理、或已知但尚未實作的部分：
 
**Mount 為 O(n)** —— 1593 筆需 1.86 s；外推 10000 筆約 11.7 s，對嵌入式裝置不可接受。
效能優化改善了常數項（變動成本降 64%），但**沒有改變複雜度**。
根本解法是在 metadata sector 建立索引，將 mount 降為 O(1)，
或改為只驗證最後 N 筆、延後全量 CRC 驗證。
 
**`log_format` 固定從 S0 開始** —— 使「繞不完的餘數」重複落在前段 sector。
在頻繁重置的情境（產線測試、韌體更新）下，前段 sector 會系統性地磨損較快。
解法是讓 format 從 erase count 最小的 sector 開始——即最小限度的 static wear leveling。
 
**GC 造成 62 ms 的 p99 長尾** —— 見〈設計決策〉。
消除它需要實作 Erase Suspend，而非單純使用背景 task。
 
**`rec_id` 為 32-bit** —— 理論上 42 億筆後繞回（以 1.4 ms/筆計算約 68 年）。
若要處理，正確做法是 TCP 式的環形序號比較（`(int32_t)(a - b) > 0`）而非直接比大小。
本系統有利條件：環形 log 中同時存在的 rec_id 範圍很窄（≤ 2000 筆），
**遠小於半週期，故環形比較的前提天然成立。**
 
**`timestamp` 使用 `HAL_GetTick()`** —— 跨重開機會歸零。真實系統需要 RTC。
 
**磨損統計的 compaction base 未持久化** —— metadata sector 寫滿後 compact 時，
「相對基準值」目前未寫入 flash，重開機後歸零。
 
**「HAL per-byte 開銷」為排除法推論** —— 未直接量測 HAL 內部，
而是「總成本減去可解釋的部分」。要確認需改用暫存器直寫版本對照。
 
---
 
## 專案結構
 
```
Core/
  Inc/  flash.h  log.h  crc32.h  wear.h  log_uart.h  cmd.h  storage.h  perf.h
  Src/  flash.c  log.c  crc32.c  wear.c  log_uart.c  cmd.c  storage.c  perf.c  main.c
tools/
  flashlog.py         host CLI（指令、benchmark、scaling）
  plot_scaling.py     mount 成本模型繪圖
  plot_gc_latency.py  tail latency 繪圖
  serial_probe.py     最小的序列埠探測工具
  data/               量測結果 CSV
docs/
  mount_scaling.png   效能分析圖
  gc_latency.png      延遲分布圖
```
 
## 版本
 
| Tag | 內容 |
|---|---|
| `v0.1-pre-gc` | 核心完成：append-only log、CRC recovery、DWT profiling |
| `v0.2-gc` | GC 完成：環形回收、磨損統計、tail latency 分析 |
 
每個 tag 對應 `tools/data/` 中該版本的量測數據，可 checkout 後重現。
