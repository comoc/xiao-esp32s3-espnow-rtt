# XIAO ESP32S3 ESP-NOW RTT 測定デモ

Seeed Studio XIAO ESP32S3 を2台使い、ESP-NOW で互いに RTT（往復時間）を測定するデモ。

## 動作

- 両方のボードに同じファームウェアを書き込む（2台構成を前提としている）
- 各ボードは1秒ごとに `PING(seq, 送信時刻)` をブロードキャスト（`FF:FF:FF:FF:FF:FF`）で送信する。相手の MAC アドレスを設定する必要はない
- PING を受け取ったボードは、seq と送信時刻をそのまま入れた `PONG` をブロードキャストで返す
- PONG を受け取ったら、`受信時刻 − 送信時刻` を RTT としてシリアルに出力し、ユーザーLED（GPIO21、アクティブLOW）を約50ms点灯する
- 次の PING までに PONG が返らなければロストとする
- 10回ごとに min/avg/max とロスト数を出力する

## 環境

| 項目 | 内容 |
|---|---|
| ボード | Seeed Studio XIAO ESP32S3 × 2 |
| プラットフォーム | espressif32 @ 6.9.0 |
| フレームワーク | Arduino（arduino-esp32 2.0.17） |
| シリアル | USB CDC、115200 bps |

## ビルドと書き込み

```sh
pio run                                  # ビルド
pio device list                          # 接続ポートの確認
pio run -t upload --upload-port COM6     # 1台目
pio run -t upload --upload-port COM7     # 2台目
```

COM 番号は環境によって変わる。

## ペイロードサイズの切り替え

送信ペイロードのバイト数はマクロ `PAYLOAD_SIZE`（9〜250、既定 9）で決まる。範囲外はコンパイルエラーになる。先頭 9 バイトが PING/PONG の中身で、残りは 0 埋め。

| env | ペイロード | 書き込み |
|---|---|---|
| `seeed_xiao_esp32s3`（既定） | 9 バイト | `pio run -t upload --upload-port COM6` |
| `payload250` | 250 バイト | `pio run -e payload250 -t upload --upload-port COM6` |

その他のサイズは環境変数で指定できる（PowerShell の例）:

```powershell
$env:PLATFORMIO_BUILD_FLAGS="-DPAYLOAD_SIZE=100"; pio run -t upload --upload-port COM6
```

## シリアルモニタ

```sh
pio device monitor -p COM6
```

出力例:

```
my MAC: XX:XX:XX:XX:XX:XX
rtt seq=11 1934 us
rtt seq=12 lost
stats n=10 min/avg/max=1879/1980/2270 us lost=1
```

実測値（2台を近くに置いた状態、60秒間、ロストはいずれも 0）:

| ペイロード | RTT 平均 |
|---|---|
| 9 バイト | 約 2.0 ms |
| 250 バイト | 約 5.9 ms |

増加分（約 3.9 ms）は、往復で増えた 482 バイトを ESP-NOW 既定の 1 Mbps で送る時間とほぼ一致する。

## 送信レートを上げる方法（未実装・未検証）

ESP-NOW の送信 PHY レートは既定で 1 Mbps（802.11b）。`esp_wifi_config_espnow_rate()`（`esp_now.h`）で変更できる。`esp_wifi_start()` の後に呼ぶ必要があり、Arduino では `WiFi.mode(WIFI_STA)` が内部で `esp_wifi_start()` を呼ぶので、`setup()` の `esp_now_init()` の後に追加すればよい。

```cpp
#include <esp_wifi.h>

// setup() 内、esp_now_init() の後
esp_err_t err = esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_24M);
if (err != ESP_OK) {
  Serial.printf("esp_wifi_config_espnow_rate failed: %d\n", err);
}
```

- 指定できる値は `wifi_phy_rate_t`（`esp_wifi_types.h`）。例: `WIFI_PHY_RATE_11M_S`（802.11b）、`WIFI_PHY_RATE_6M`〜`WIFI_PHY_RATE_54M`（802.11g）、`WIFI_PHY_RATE_MCS0_LGI`〜`WIFI_PHY_RATE_MCS7_SGI`（802.11n）
- 設定は送信側にだけ効く。このデモは2台とも送信するので、両方に同じ設定を入れる
- レートを上げるとデータ部分の送信時間は縮むが、プリアンブルや処理時間は変わらないため、RTT がレートに比例して縮むわけではない
- 高いレートほど電波の届く距離が短くなり、ロストが増えやすい
- このプロジェクトの arduino-esp32 2.0.17（ESP-IDF 4.4）で確認した API。arduino-esp32 3.x（ESP-IDF 5.x）ではピアごとの `esp_now_set_peer_rate_config()` に置き換わっている可能性があるため、コアを上げる場合はヘッダを確認すること

## 実装メモ

- ESP-NOW の受信コールバックは Wi-Fi タスクで実行される。コールバック内で `Serial` に直接出力すると、出力の一部が欠けることがあった。そのため、コールバックでは受信時刻を記録してイベントを FreeRTOS キューに入れるだけにし、PONG の返信、出力、LED の制御は `loop()` で行っている
- そのため、RTT には相手側でキューを経由して `loop()` が PONG を返すまでの時間も含まれる
- ブロードキャストでは MAC 層の ACK や再送がないため、再送による遅延は含まれない
- 片道遅延は2台の時計がそろっていないと測れないため対象外。目安が必要なら RTT÷2 を使う

## トラブルシューティング

- **書き込み時に `No serial data received` が出る**
  BOOT ボタンを押したまま RESET ボタンを押し、BOOT を離して書き込みモードにしてから再実行する。書き込み後は RESET ボタンを押して起動する
- **`could not open port ... Access is denied`**
  シリアルモニタなど、他のプログラムがそのポートを開いている。閉じてから再実行する
- **エディタに `'Arduino.h' file not found` などの赤線が出る**
  ビルドには影響しない。`pio run -t compiledb` で `compile_commands.json` を生成すると解消する
