# 検証記録と実機試験手順

日付: 2026-10-26。**ソフトウェア試験済み、実機通信・電気特性・適合性は未検証**。
商用センサーは未選定で、TIOX1X2XEVMや接続先機器の購入・接続は行っていません。

## 実行環境

- Arduino CLI、ローカルに導入されていたArduino-ESP32 3.3.11、ArduinoJson 7.4.3。
- ネイティブ試験: Windows x64、MSVC 19.51、CMake 4.3.1。
- Python 3.12.14、pyserial 3.5。Pythonパッケージのwheel作成・ローカルインストールを確認。
- この環境のサンドボックス内ではEspressifコンパイラーがWindows DLL relocationで起動失敗。
  通常のローカル実行ではコンパイルできました。ソースのコンパイルエラーとは区別しています。

## 確認済み

| 項目 | 方法と結果 |
| --- | --- |
| XIAO ESP32C3ビルド | Arduino-ESP32 3.3.11、CDCOnBoot=defaultで成功。スケッチ320,494バイト、静的RAM14,640バイト |
| ESP32-S3ビルド | 同コア、USBMode=hwcdc、CDCOnBoot=cdcで成功。スケッチ335,414バイト、静的RAM23,616バイト |
| 起動と能力選択 | 模擬PHYでCOM3→COM2→COM1、1回のWake-up後の速度検出とOPERATE遷移 |
| 機器情報 | Page 1からVendorID/DeviceID/PD長/周期を取得 |
| 周期データ | 1/2/4/32バイト入力、長いMinCycleTime、PD無効フラグを確認 |
| ISDU | COM1/2/3、16bit Index・Subindex、232バイト拡張長の読み書き・読戻し |
| エラーと復帰 | 機器のErrorType保持、チェックサム異常、応答欠落、途中で切れた応答、再接続 |
| 再送 | ISDU応答欠落時に同一要求を再送することを模擬トランスポートで照合 |
| 不正ISDU・Busy | 不正ExtLengthを拒否、Busy待ち時間切れ、エラー後の次サービスを確認 |
| カウンター周回 | 32bit時刻が周回する起動を確認 |
| バッファと固定ベクトル | PD取得時の容量不足を拒否。A.1.6式から求めた固定チェックサム値を照合 |
| USB | 1文字ずつの受信、不正コマンド・Index・Hex、過大行、末尾の余分なJSON・生CRの拒否、返信待ち中の周期通信継続 |
| USBセッション | 部分行・未送信応答の破棄と再利用をネイティブエンドポイントで確認 |
| Python | 11試験。分割応答、旧ID無視、書込み非再送、範囲検証、CLIのHex指定・有限ストリーム |
| Python⇔C++ | 同梱C++エンドポイントのプロセスに接続し、読出し→232バイト変更→再読出し→復元を確認 |

2026-10-26の検証時は、CTestの`integration`と`python`の2ターゲットが成功しました。
2026-10-28にPythonクライアントを独立リポジトリ `lwiolink-python` へ切り出したため、
現在のCTestはC++の`integration`のみです。上のPython関連の結果は切り出し前の検証記録です。
ネイティブの模擬応答は実機の電気的遅延やUART割込み遅延を再現しません。
同系統の共有フレームヘルパーを使う模擬機器との一致だけでは、独立した機器との互換性は証明できません。
実UARTのパリティ・フレーミング異常検出、USBバスリセットイベント、タスクの最悪遅延は実機確認が必要です。

## 再現

```powershell
./tools/build.ps1 -Board all -ArduinoJson "D:/path/to/ArduinoJson"
cmake -S . -B build/native -DARDUINOJSON_INCLUDE="D:/path/to/ArduinoJson/src"
cmake --build build/native --config Debug
ctest --test-dir build/native -C Debug --output-on-failure
```

Python試験は独立リポジトリ `lwiolink-python` 側で実行します。
結合試験は `IOLINK_TEST_SERVER` に、このリポジトリでビルドした
`integration_tests` 実行ファイルの絶対パスを指定します。
詳細は `lwiolink-python` のREADMEを参照してください。
このリポジトリのCMake設定・ビルド・CTestはPythonを必要としません。

## 実機での受入試験（未実施）

1. センサーの型番、IODD版、VendorID/DeviceID、COM速度、MinCycleTime、PD形式、
   使用する書換可能Index/Subindexを記録する。初版の対応条件は`bom.md`で確認する。
2. 電源断状態で配線とジャンパーを確認。USB給電後、ENがLow、外部24Vからの逆給電がなく、
   ロジック電圧が3.3Vであることを確認する。
3. MCU TX/RX/ENと、適切な電圧定格のプローブでCQを観測する。
   `start`時のWake-up要求が75～85µsであることを測定し、CQの極性と電流も確認する。
4. 該当COM速度で8E1の波形、TX/RX切替、自己エコーの除去、応答全体の受信を確認する。
   特にCOM3の最短応答で先頭バイトを失わないことを確認する。
5. 取得した機器情報をIODDと照合。MasterCycleTime書込みと実際の周期を測り、
   機器最小周期を下回らないこと、ISDU中も周期入力が更新されることを確認する。
6. 機能を変えない書換可能項目の元値を保存し、変更・再読出し・元値への復元・再読出しを行う。
   書込みが不明になった場合は再送前に現在値を確認する。
7. 受信するPCアプリを閉じ、MCUへ給電を維持した状態で周期通信が続くことを測定する。
   再接続で書込みが再実行されないことを確認する。
8. センサーの抜き差し・電源再投入で、古いPDが有効として返らず、機器情報を取り直して復帰することを確認する。
9. 1時間の連続取得でチェックサム、タイムアウト、周期遅延、再接続回数を記録する。
   UART異常は測定器または信号シミュレーターで注入し、24V系の意図的な短絡試験は行わない。

これらの実測結果を追記するまでは、完成条件のうち実機での測定値取得と設定復元は未達です。
