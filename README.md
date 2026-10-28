# lwIOlinkMaster — Arduino USB IO-Link master prototype

ESP32-C3 / ESP32-S3とTIOL112を使う、1ポートのUSB IO-Linkマスター試作です。
USB CDCから周期入力データとISDU読み書きを利用できます。**商用センサーと実PHYでの動作・波形・適合性は未検証**です。

## 構成

```text
Python / PC ── USB CDC ── ESP32-C3/S3 ── UART1 + EN ── TIOL1123 EVM ── IO-Link sensor
                         USB給電                       外部24V給電
```

- 基準ボード: Seeed XIAO ESP32C3。S3の接続例はESP32-S3-DevKitC-1。
- Arduino-ESP32 **3.3.11**、ArduinoJson **7.4.3**。
- COM3 → COM2 → COM1の自動検出、Direct Parameter Page 1の読出し、周期設定とOPERATE遷移。
- 対象: IO-Link RevisionID 0x11、Class A、周期出力なし、OPERATE M-sequence code 0 / 4 / 5。
  入力は最大32バイト。ただしcode 0は最大2バイト、code 4は3バイト以上。
  ISDUは対応機器で最大232バイトの読み書き。
- Type 1のインターリーブ、OD 8/32バイト、周期出力、Data Storage、認証は対象外。
- センサー値は生バイトです。単位・符号・倍率は対象機器のIODDに基づきPC側で変換してください。

## Arduinoでビルド

このフォルダーをArduinoライブラリとして追加し、ArduinoJson 7.4.3をインストールします。
`examples/USB_IOLinkMaster/USB_IOLinkMaster.ino`を開いてください。

| ボード | USB設定 | FQBN |
| --- | --- | --- |
| XIAO ESP32C3 | USB CDC On Boot: Enabled | `esp32:esp32:XIAO_ESP32C3:CDCOnBoot=default` |
| ESP32-S3 Dev Module | Hardware CDC and JTAG、USB CDC On Boot: Enabled | `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc` |

**C3の`CDCOnBoot=cdc`は、この固定コアではDisabledです。** S3とは選択値が異なります。
S3はUSB-UART変換器側ではなく、MCUのネイティブUSB端子を使います。
書込みは通常のArduino IDE/CLIから行ってください。起動直後はポート停止状態です。

Windowsの再現用ビルド:

```powershell
./tools/build.ps1 -Board all -ArduinoJson "D:/path/to/ArduinoJson"
```

コアのインストール例:

```text
arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install ArduinoJson@7.4.3
```

接続前に[配線・電源・ジャンパー設定](docs/hardware.md)を確認してください。
[部品と費用](docs/bom.md)、[検証結果と実機試験手順](docs/validation.md)も参照できます。

## PC用CLI・GUI

Pythonクライアント、CLI、GUI、IODD処理、Windows用GUIビルドツールは、
独立リポジトリ `lwiolink-python` に切り出しました。
インストール・操作・GUIビルドの手順は、そちらのREADMEを参照してください。

ArduinoファームウェアとPCソフトは、[USB protocol v1](docs/usb-protocol.md)で通信します。
このリポジトリのArduinoビルドとネイティブ試験は、Pythonを必要としません。

## ライブラリAPIと検証

新しい通信APIは`SinglePort`、USBエンドポイントは`UsbProtocol`です。
`SinglePort`とPHYは同じ通信タスクから操作し、USB側とはキューで接続します。
既存の`Master` / `Port`はPHY初期化用の旧APIとして残しています。
TIOL112ドライバーはUART1を直接所有します。`Serial1.begin()`と併用しないでください。

PC上の模擬PHY試験:

```text
cmake -S . -B build/native -DARDUINOJSON_INCLUDE=/path/to/ArduinoJson/src
cmake --build build/native --config Debug
ctest --test-dir build/native -C Debug --output-on-failure
```

PCクライアントの試験は `lwiolink-python` 側で実行します。
PythonとC++エンドポイントの結合試験には、このリポジトリで生成した
`integration_tests` のパスを環境変数 `IOLINK_TEST_SERVER` で渡してください。
両リポジトリのソースを直接参照する必要はありません。

## ソースとライセンス

このプロジェクトはGPL-3.0-onlyです。通信コアには、固定コミットの
[iolinki-master](https://github.com/w1ne/iolinki-master)と
[iolinki](https://github.com/w1ne/iolinki)のGPL互換ソースを同梱しています。
コピー元、固定コミット、ローカル補修は[third_party/README.md](third_party/README.md)に記録しています。
実機での相互運用性は今後の検証項目です。
