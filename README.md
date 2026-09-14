# ESP32 低音騒音モニタリングシステム

ESP32とI2Sマイク（INMP441）を使用し、30Hz〜100Hzの低音域騒音をリアルタイムに解析・Webブラウザ上に可視化するシステムである。

## 概要
* **マイコン:** ESP32 (FreeRTOS / デュアルコア活用)
* **センサー:** INMP441 (I2S MEMSマイク)
* **主な機能:** 
  * サンプリング周波数 8kHz / 1024サンプルによる FFT 解析（分解能 7.8125Hz）
  * Core 0（Web通信）と Core 1（音声解析）の分離処理
  * Mutex（セマフォ）によるコア間データの排他制御
  * JavaScript (Fetch API / Chart.js) によるリアルタイムグラフ表示

## ファイル構成
* `src/main.ino` : ESP32用メインソースコード
* `src/index_html.h` : WebUI用HTML/JSコード
