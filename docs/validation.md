# 検証記録と実機試験手順

更新日: 2026-10-28。**PC上のソフトウェア試験とESP32-C3実機のUSB通信を確認済み。
IO-Link実機通信・電気特性・適合性は未検証**。
商用センサーは未選定で、TIOX1X2XEVMや接続先機器の購入・接続は行っていません。

## 2026-10-28 追加レビュー4件の改善

- 診断イベントをSinglePortで自動収集し、保存後にセンサーへ確認します。
  USBの`status`で未取得件数・破棄件数・確認待ちを通知し、`events`で最大6件ずつ取得できます。
  16件のキューが満杯なら最古を破棄して累計件数へ記録。USB切断・ポート停止・再確立でもキューを保持します。
- 診断確認の応答欠落・チェックサム異常でも同じWRITEを再送。
  既に読み取った警告は確認応答を失っても保存し、コールバックを重複させません。
  ISDUの応答が飛行中に診断読出しを開始しても、その応答をISDUへ返します。
  OD 2バイトの診断はOD0だけを評価し、後続ODがゼロでも全アドレスを読みます。
- ISDU全体の固定30秒上限を、現在の周期・OD幅に応じた予算へ変更しました。
  最低30秒とし、最大PDU、応答準備待ち、2回の再送、診断1バッチと実装の余裕を加算します。
  `isduTimeout100us()`／`status.isdu_timeout_us`をPC側の要求期限の設定にも利用してください。
- チェックサム異常時の再送カウンターは要求期限の処理だけで加算し、二重消費を解消。
  品質比率の乗算・カウンター合算を64bitに変更しました。

`tests/runtime_regressions.cpp`に9項目を追加し、既存14項目と合わせてCTest 23/23が成功しました。

ArduinoJson 7.4.3と導入済みArduino-ESP32 **3.3.12**で、以下のビルドも成功しました。

| ボード | FQBN | スケッチ | 静的RAM |
| --- | --- | --- | --- |
| XIAO ESP32C3 | `esp32:esp32:XIAO_ESP32C3:CDCOnBoot=default` | 323,600バイト | 14,640バイト |
| ESP32-S3 | `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc` | 338,346バイト | 23,616バイト |

この時点では固定版3.3.11が環境からなくなっており、`tools/build.ps1`は前提チェックで停止しました。
そのため同じFQBNでArduino CLIを直接実行し、3.3.12で検証しました。
今回の変更を3.3.11で再ビルドした結果や、実機へ書き込んだ結果ではありません。

| 項目 | 確認内容 |
| --- | --- |
| events_details | 詳細イベント6件の自動収集・確認・USB取得。OD 1/2、PDなしType 0、時刻周回、周期入力と元のコールバック継続 |
| events_legacy | 旧StatusCodeの変換順・EventQualifier・診断要求がアドレス0だけであること |
| events_failures | 診断読出しと確認WRITEに2回の欠落／破損を注入して復帰。3回失敗後も保存した警告を保持し、再確立後の次イベントを取得 |
| events_overflow | USB返信待ち中の周期継続、18件発生時の16件保持・2件破棄、USB切断／stop後の保持 |
| events_isdu | ISDU応答の途中に診断6件を発生させても、OD 1/2でデータが一致 |
| slow_max_cycle | 132.8ms周期・232バイト・OD 1/2の読書きと4.4秒のBusy。30秒超でも成功。時刻周回とUSB期限表示 |
| checksum_retries | DeviceOperate・ISDU要求／応答・周期／完了でチェックサム2回異常後に成功。3回なら再確立 |
| quality | 42,949,673／50,000,000／UINT32_MAX周期でエラーなし100%。エラー合算が32bitを超える場合も正しく算出 |
| event_c_api | C APIでの読出し・確認。容量0の拒否、小容量の必要件数返却、結果再取得、通知重複なし |

品質試験はカウンター値を設定した算術試験です。24時間以上の実機連続運転ではありません。
イベントの電気的なタイミング、センサーとの相互運用性、最悪タスク遅延は実機未検証です。
PCクライアントは別リポジトリのため今回変更していません。既定35秒で不足する用途はPC側の期限設定も必要です。
仕様根拠は[IO-Link V1.1.3](https://io-link.com/fileadmin/user_upload/Downloads/Package_2020/IOL-Interface-Spec_10002_V113_Jun19.pdf)の
7.3.8.2、A.2.3/A.2.4、A.6.2、A.4.1.1/A.4.3.1です。

## 2026-10-28 ESP32-C3単体のUSB実機検証

LOLIN C3 MiniをCOM3へ接続し、`USB_IOLinkMaster`のビルド・書込み・USB通信を確認しました。
TIOL112・外部24V電源・IO-Link機器は接続していません。

| 項目 | 条件・結果 |
| --- | --- |
| 実機 | ESP32-C3 revision v0.3、内蔵フラッシュ4MB、USB Serial/JTAG |
| ビルド | Arduino-ESP32 3.3.12、ArduinoJson 7.4.3 |
| ボード設定 | ESP32C3 Dev Module、`esp32:esp32:esp32c3:CDCOnBoot=cdc` |
| 使用量 | スケッチ342,268バイト、静的RAM15,740バイト |
| 書込み | COM3へ成功、書込みデータのハッシュ照合成功 |
| 起動後 | `status`が`inactive`、`protocol=1`、`busy=false`、`last_error=0` |
| 未接続時の要求 | `info`・`pd`・ISDU読書きが未接続エラーを返す |
| 入力処理 | CRLF、1文字ずつの分割受信、768バイトの最大長要求を受理 |
| 不正入力 | 未定義操作・キー、不正Index・Hex、末尾の余分な文字を拒否 |
| 過大入力 | 769バイトの行を拒否し、次の正常な要求に応答 |
| 連続応答 | `status` 500回すべて成功。PC側計測で平均3.17ms、最大9.00ms |
| ポート再オープン | PC側でCOM3を閉じて開き直す操作を5回行い、毎回正常応答 |

`tools/usb_smoke.py`の18項目がすべて成功しました。応答時間はこのPCでの短時間の実測値で、
IO-Linkの周期性能や最悪遅延を示すものではありません。`start`は送信せず、
試験終了時もポートは`inactive`です。ポート再オープンはUSBケーブルの抜き差しや
USBバスリセットの試験とは別です。UART送受信・Wake-up・C/Q波形・センサー通信・
長時間安定性は今回の確認に含みません。

再実行にはPythonとpyserialが必要です。ファームウェアを起動直後の停止状態にし、
Arduino IDEのシリアルモニターなどCOM3を使用するアプリを閉じて実行します。
このスクリプトはPC側の試験用であり、通常のArduinoビルドには不要です。

```powershell
python tools/usb_smoke.py --port COM3 --output build/hardware-c3-usb-results.json
```

今回の生応答記録は`build/hardware-c3-usb-results.json`（Git管理対象外）へ保存しました。
書き込んだアプリケーションバイナリーのSHA-256:
`6ab2cf28a7c43c80722f394e6786681629476cceed267e17c7e6f6ddc7e53f1f`。
従来の3.3.11によるビルド結果とは条件を分けて記録しています。

## 2026-10-28 高優先度レビュー指摘の修正

起動中の要求間隔、UART受信異常からの復帰、MasterIdent未送信の3点を修正しました。
実機での相互運用性・波形は引き続き未検証です。

- 起動時の全要求と再送を100 T_BIT以上の間隔に制限し、100µs時刻の丸めに対して1 tickを加算。
  パラメータ取得中も次回送信可能時刻をスケジューラーへ通知します。
- 自動設定では通信パラメータ0x02～0x06を読んだ後、MasterIdent (0x95)を送信。
  CKSのみの応答を検証するまで機器識別の読出し・周期設定・OPERATEへ進みません。
  リンクを再確立した場合も同じ手順をやり直します。
- TIOL112のUARTパリティ／フレーミング／バッファ異常は、一時的な受信エラーとして破棄・再送。
  再送上限を超えた場合はリンクを再確立します。nFAULTやドライバー障害はERRORで停止します。
  外部PHYアダプターでは、復帰可能な受信異常に`IOLINK_PHY_RX_ERROR` (-2)を返してください。
  その他の負値は従来どおりハードウェア／ドライバー障害として扱います。

`tests/startup_recovery.cpp`に7項目を追加しました。既存の`integration`と合わせてCTestは8項目です。

| 追加項目 | 確認内容 |
| --- | --- |
| timing | COM1/2/3で要求開始間隔、MasterIdentの応答確認、32bit時刻周回 |
| ident_retries | MasterIdent応答の欠落・チェックサム破損からの再送。全COM速度で確認 |
| scan_errors | 異なるCOM速度でUART異常が発生しても探索を続け、1回のWake-upで確立 |
| transient_errors | Page読出し・MasterIdent・周期通信中のUART異常から復帰。異常直後のPD無効化 |
| repeated_errors | UART異常が続くと2回再送後に再確立。古いPD・機器情報の無効化とMasterIdentの再実行 |
| partial_isdu_errors | ISDU要求・応答の途中でUART異常を注入し、同一フレームの再送とデータ取得を確認 |
| fatal_error | 恒久的なPHY障害では送信停止し、明示的なstartで再開 |

2026-10-28の修正後検証ではCTest 8/8が成功しました。Arduino-ESP32 3.3.11 / ArduinoJson 7.4.3で、
XIAO ESP32C3（スケッチ320,668バイト、静的RAM14,640バイト）とESP32-S3
（スケッチ335,602バイト、静的RAM23,616バイト）のビルドが成功しました。

規格根拠は[IO-Link Interface and System Specification V1.1.3](https://io-link.com/fileadmin/user_upload/Downloads/Package_2020/IOL-Interface-Spec_10002_V113_Jun19.pdf)の
A.2.6/A.3.9、A.4.1、9.2.3.3 Table 86 T21、B.1.2です。
この高優先度修正では中優先度のISDUタイマー・完了IDLE・拡張長境界を対象外としました。
その後の対応は次節を参照してください。

## 2026-10-28 中優先度レビュー指摘（P2 4～6）の修正

- ISDUTimeの監視対象を応答準備待ち（WAIT）に限定し、応答開始時に停止。
  分割転送中は各M-sequenceの応答期限と再送・リンク再確立で監視します。
- 完了結果の取得直後に次の読出し／書込みを要求しても、前サービスのIDLE／ABORTを保持。
  完了フレームの応答を確認してから次サービスを初期化します。
  完了応答の欠落・チェックサム異常でも同じIDLE／ABORTを再送します。
- 書込みの長さ判定をExtLengthを含む実送信長へ変更。
  ExtLength追加前が16バイトなら、正当な全長17バイトとして受け付けます。
  バッファ容量と全長238バイトの上限も検査します。
- 模擬機器はSTARTで前要求のODパディングを破棄し、IDLE／ABORTで転送状態を解放。
  回帰試験のトランスポートは、完了フレームを挟まない次サービスを拒否します。

`tests/isdu_regressions.cpp`に6項目を追加しました。MSVC DebugビルドとCTest 14/14が成功しました。
修正前のコードでは、このうち長周期転送・Busy後の次要求・連続IDLE／ABORT・書込み長境界の
5項目が失敗することを確認しました。

| 追加項目 | 確認内容 |
| --- | --- |
| slow_response | 周期32ms・OD 1/2バイト・232バイト読出し。4.5秒のBusy後に応答を開始し、総転送が5秒を超えても成功。時刻周回も確認 |
| busy_timeout | 設定した待ち期限で0x1100を返し、直後の次要求より先にABORTを送信。時刻周回も確認 |
| completion_idle | 結果取得直後の読出し→読出し→書込み→読出し、機器エラー後の書込みにIDLEを挿入。OD 1/2バイト、PDなしType 0、完了応答の欠落・破損も確認 |
| completion_abort | 不正応答後の次要求の前にABORTを送信。上と同じプロファイルで完了応答の欠落・破損から再送 |
| write_lengths | 3種類のIndex形式で全長15/17/18/238バイト、ヘッダー・データ・CHKPDUを照合し、239バイトを拒否 |
| segment_timeout | 応答開始後のセグメントが途絶えても同一フレームを2回再送し、再確立後に次サービスが成功 |

長さ試験の238バイトPDUのうち、8bit Index形式の233/234バイトデータはC APIで送信形式を確認します。
模擬機器のオブジェクト上限は232バイトのため、この2ケースでは機器エラー応答を期待します。
SinglePortの公開上限は従来どおり232バイトです。

Arduino-ESP32 3.3.11 / ArduinoJson 7.4.3で、XIAO ESP32C3
（スケッチ320,790バイト、静的RAM14,640バイト）とESP32-S3
（スケッチ335,798バイト、静的RAM23,616バイト）のビルドが成功しました。
サンドボックス内のEspressifコンパイラーはDLL relocationで起動できなかったため、
同じビルドコマンドを通常のローカル環境で実行しました。

規格根拠は[IO-Link Interface and System Specification V1.1.3](https://io-link.com/fileadmin/user_upload/Downloads/Package_2020/IOL-Interface-Spec_10002_V113_Jun19.pdf)の
7.3.6.3 Table 53 T4/T6/T8/T11、7.3.6.4 Table 54、A.5.3 Table A.14です。
実機での長周期転送・連続サービス・通信ノイズからの復帰は未検証です。

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
2026-10-28のレビュー修正後のCTestはC++の14項目です。上のPython関連の結果は切り出し前の検証記録です。
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
