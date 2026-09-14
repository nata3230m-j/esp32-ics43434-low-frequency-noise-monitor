#include <Arduino.h>
#include <driver/i2s.h>
#include <math.h>
#include<arduinoFFT.h>//FFTを使うためのライブラリ
#include <WiFi.h>       // Wi-Fiつなぐため
#include <WebServer.h>  // リクエストに対して適切に返答するため
#include "index_html.h" // 体裁を書いておいたファイル


//WiFi設定 -> 記載のまま提出してはいけない！
const char* ssid = "Enter your SSID";//このマイコンは2.4GHZのみ対応
const char* pw   = "Enter your Password";

// I2Sピン設定
#define I2S_WS   21 //ワードセレクト：左右チャンネル区別
#define I2S_SD   32 //シリアルデータ
#define I2S_SCK  22 //シリアルクロック
#define I2S_PORT I2S_NUM_0 //I2S0を使用

// サンプル取得の設定
#define SAMPLE_RATE 8000//低音細かく拾いたいから下げる
#define BLOCK_SIZE  1024//FFTサンプル数(2^n)

//うるさいと判断する閾値
#define BASS_THRESHOLD 28.0//とりあえず35固定
#define DIFF_THRESHOLD 15.0   // 全体との許容差 [bass >= total - 15.0]
#define REQUIRED_DURATION_MS 500   // 持続判定時間 (0.5秒)

//log,平滑化関数の宣言
double line_To_log(double line_vol, double &smoothed_ref);

//Core 0 で動くWeb通信処理関数の宣言
void TaskWebCommu(void *pvParameters);


//FFT用の配列とオブジェクトの設定
double vReal[BLOCK_SIZE];//cos成分.波の単純な大きさ
double vImag[BLOCK_SIZE];//sin成分.位相の情報(切り取るタイミングのズレ補正.毎秒1024しか見てないから)
ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal,vImag,BLOCK_SIZE,SAMPLE_RATE);//FFTクラスオブジェクトをインスタンス化。


// Webサーバーオブジェクト(port=80)
WebServer server(80); // 直接初期化

//別コア共有変数・セマフォ用変数の宣言
volatile float g_bass_db   = 0.0;
volatile float g_total_db  = 0.0;
volatile bool  g_is_alert  = false;
SemaphoreHandle_t g_data_mutex;


// タイマー管理用変数
unsigned long sound_start_time = 0;//計測開始時刻
bool is_alert_active = false;//警報発報してるか


void setupI2S() {//I2S通信の初期設定。 書式→ .設定項目名=値 どの項目に名に設定するかを名前付きで明記
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),//マイコンが、マスター(clk生成)かつマイクよりデータ受信(RX)という動作
    .sample_rate = SAMPLE_RATE, //44100に設定。
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT, // ICS43434は32bitフォーマットでデータ出力。あとで24bitに直す
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, //左チャネルのみから取得
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,//通信手順と波形・タイミングを標準i2s規格とする
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,//割り込み優先度Lv.1(通常の優先度)
    .dma_buf_count = 4, //DMA(データをCPUの代わりにRAMに運ぶ.cpuを介さない)リンクバッファの数:4(1つの箱に512サンプル詰める)→取りこぼし防止と計算中のバックグラウンドサンプリングのため.DMAで詰まるのが嫌だからバッファをいくつか。
    .dma_buf_len = BLOCK_SIZE,//DMA:リンクバッファの大きさ=ひとまとめの大きさ。1024サンプルを1まとまりで扱う.
    .use_apll = false//高精度オーディオクロック不使用
  };

  i2s_pin_config_t pin_config = {//ピン設定.上記でdefineしたやつ。
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,//（スピーカ）出力は未使用。ピン節約できる
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);//i2s回路の起動・通信ルールとdmaの確保.i2s回路0を使う(defineしたやつ),上記設定の構造体を読む、単純な入力のみで割り込みなし。
  i2s_set_pin(I2S_PORT, &pin_config);//i2s num 0回路とピンを結線。
}

void setup() {


  Serial.begin(115200);//通信速度設定
  setupI2S();//いろいろ設定（上記）
  g_data_mutex = xSemaphoreCreateMutex();//起動時にセマフォ初期化。→CORE0通信タスク、CORE1 loop()が走る前に生成してないとクラッシュする
  if(g_data_mutex == NULL){
    Serial.println("ERROR!! セマフォの作成失敗!");
    while(1);{delay(1000);}//無限ループで固める
  }
  Serial.println("ICS43434 騒音計テスト開始(低域)");

  //CORE0タスク開始->コア指定してTaskWebCommu関数を開始できる
  xTaskCreatePinnedToCore(TaskWebCommu, "WebTask", 8192, NULL, 1, NULL, 0);//タスク関数名,タスク名,スタックサイズ,パラメータ,優先度,タスクバンドル,コアID
}

void loop() {//CORE1側の処理。サンプリング・FFT・セマフォ書き込み
  int32_t samples[BLOCK_SIZE];//符号付整数32bit.生のデータを入れる。
  size_t bytes_read = 0;//符号なし整数

  // I2Sからデータを取得
  i2s_read(I2S_PORT, &samples, sizeof(samples), &bytes_read, portMAX_DELAY);
  //&samples:samplesに読んだデータを詰めたい。&bytes_read:読んだバイト数を返してもらう.sizeof(xxx):バイト数

  //波形データをFFT配列(vReal)にセットと、直流成分除去-> 波形を上下にずらして、中心を0に合わせる。(振動の中心は平均。普通0のはず)
  int samples_read = bytes_read / sizeof(int32_t);
  double mean = 0.0;//直流オフセット除去のため、平均をひく、そのための変数
  if (samples_read == BLOCK_SIZE) {//DMAからデータをすべてピッタリ読み取ったか？
    //波形データをFFT配列(vReal)にセット
    for(int i = 0; i<BLOCK_SIZE; i++){
      int32_t sample = samples[i] >>8;//24bitに。// 32bitデータから24bit分を取り出し（24bit精度マイクで、上位から送られてくるため。）
      vReal[i] = (double)sample / 8388608.0;//正規化(±1)
      mean += vReal[i];
      vImag[i] = 0.0;//虚数部。0にリセット
    }

    // 平均値を引いてDC成分を取り除く
    mean /= BLOCK_SIZE;//ここで平均値に.
    for (int i = 0; i < BLOCK_SIZE; i++) {
      vReal[i] -= mean;
    }

    //FFT解析
    FFT.windowing(FFTWindow::Hamming,FFTDirection::Forward);//端っこ不連続を滑らかに.ハミング窓:切り出したデータの両端を、徐々に0になるよう掛け算する窓。
    FFT.compute(FFTDirection::Forward);//周波数分解.時間軸→周期枢軸へ順変換(逆フーリエではない。)
    FFT.complexToMagnitude();//複素数を、(√実部^2+虚部^2 )と計算して振幅に変換。vReal[i]に上書き

    //低音域を抽出(30~101Hz)
    //1Bin当たりの周波数：8000/1024 = 7.8125Hz
    //Bin 1:~7.8125Hz, Bin4:31.24Hz...Bin13:101Hz;
    double bass_vol = 0;
    double max_bass = 0;//30~100Hzで最大の周波数（要はピーク）を格納
    int max_bass_index = 0;//ピークのインデックス
    for (int i = 4; i <=13;  i++){//30Hz~100Hzの、線形的なエネルギー量の合計を出す
      bass_vol +=vReal[i];
      if(max_bass < vReal[i]) {//その周波数の音が最大の場合は
      max_bass = vReal[i];//より大きいのにピークの更新
      max_bass_index = i;//インデックス更新
      }
    }

    //全体の大きさ：比較用に計算
    double total_vol = 0.0;
    for(int i = 1; i<(BLOCK_SIZE/2);i++){
      total_vol +=vReal[i];
    }

    //dbにして表す(線形->対数) 
    static double smoothed_val_bass = 0;
    static double smoothed_val_max_bass = 0;
    static double smoothed_val_total = 0;//static:最初の1回だけ初期化。2回目以降はそのまま値を持ち続ける

    //log,平滑化結果を格納
    double bass_result   = line_To_log(bass_vol, smoothed_val_bass);
    double max_bass_result   = line_To_log(max_bass, smoothed_val_max_bass);
    double total_result = line_To_log(total_vol, smoothed_val_total);



    //騒音判定
    bool bass_absvol_judge = (smoothed_val_bass >= BASS_THRESHOLD);//絶対的に超えたか？
    bool tolerance = ((smoothed_val_total-smoothed_val_bass)<=DIFF_THRESHOLD);//全体から10dbいないまで迫っているか
    bool is_out= (bass_absvol_judge && tolerance);//上記をどっちも満たす？

    unsigned long now = millis();

    if(is_out){//閾値超えて、全体と比べても大きいか？
      if(sound_start_time == 0){//計測開始時刻未設定なら
        sound_start_time = now;//計測開始時刻を現在に
      }
      if((now - sound_start_time) >= REQUIRED_DURATION_MS){//1秒以上続いたとき
        if(!is_alert_active){ //アラート未発動→発動
          is_alert_active = true;
          Serial.println("騒音発生！");
          Serial.printf(" 低音音量: %.2f dB | ピーク: %.2f Hz: %.2f db| 全体音量: %.2f dB | 継続: %.2f秒\n", 
                        smoothed_val_bass, (max_bass_index*7.8125),max_bass_result,smoothed_val_total, (now - sound_start_time) / 1000.0);
        }else{//すでにアラート発動中の場合
          // アラート発動中の継続表示
          Serial.printf("  [騒音検知中] 低音音量: %.2f dB | ピーク: %.2f Hz: %.2f db| 全体音量: %.2f dB | 継続: %.2f秒\n", 
                        smoothed_val_bass, (max_bass_index*7.8125),max_bass_result,smoothed_val_total, (now - sound_start_time) / 1000.0);
        }
    }else{
      //まだ1秒未満のとき
      Serial.printf(" [低音検知... %.3f秒経過]  低音音量: %.2f dB | ピーク:%.2f Hz: %.2f db| 全体音量: %.2f dB \n", 
                        (now - sound_start_time)/1000, smoothed_val_bass, (max_bass_index*7.8125),max_bass_result,smoothed_val_total);
      }
    
    }else{//騒音が収まったとき
      if(is_alert_active){
      Serial.println("騒音が収まった");
      
      is_alert_active = false; //アラート発砲しているか？をfalseに
      }
      sound_start_time = 0 ; //条件以外では常に0

      // 平常時ログ表示
      /*Serial.printf(" 正常 |  低音音量: %.2f dB | ピーク: %.2f Hz: %.2f db| 全体音量: %.2f dB\n", 
                        smoothed_val_bass, (max_bass_index*7.8125),max_bass_result,smoothed_val_total);*/
    }
    
    //共有変数への書き込み
    xSemaphoreTake(g_data_mutex, portMAX_DELAY); //変数触るとき、他のコアを待たせる
    g_bass_db  = (float)smoothed_val_bass;    //共有変数書き換え(ここから3行)
    g_total_db = (float)smoothed_val_total;
    g_is_alert = is_alert_active;
    xSemaphoreGive(g_data_mutex);                // 終わったから、他のコアがいじって良くなる

    //出力
    /*Serial.print("低音域(31.24~101Hz):");
    Serial.print(bass_result,2);
    Serial.print("  |  低音でのピーク:");
    Serial.print(max_bass_index*7.8125,3);
    Serial.print(" Hz ");
    Serial.print(max_bass_result,2);
    Serial.print("  |  全体 :");
    Serial.println(total_result,2);
    */
  
  }

  //delay(100);//100ms
}


//log化平滑化関数
double line_To_log(double line_vol, double &smoothed_ref){
    //log化
    double log_vol = 20.0 *log10(line_vol+1e-6) + 27.0;//db公式:エネルギ10倍で+20db ・1e-6:0除算防止。影響しない小さい数,＋27マイナス値底上げ。ほんとはキャリブレーション必須.携帯のdb計で代用te
    if(log_vol < 0) log_vol = 0;//dbマイナスなら0にする

    
    smoothed_ref = (smoothed_ref * 0.3) + (log_vol * 0.7);//過去：今＝7:3では、レスポンス速度0.4秒になる。Fastとslow特性の間にできる(0.125sと1sの間)


return smoothed_ref;//log化して、平滑化したのを返す

}

//CORE0用関数。Web処理
void TaskWebCommu(void *pvParameters) {//void *pvParameters引数：xTaskCreatePinnedToCore でタスクとして起動する関数には必要。汎用ポインタを受け取る→何も返さないvoid関数ってルール。
  WiFi.begin(ssid, pw);//指定したSSID,PWで接続開始。
  WiFi.setSleep(false);//WiFi省電力モードOFF->応答性重視
  Serial.print("[Core 0] Connecting to Wi-Fi");//起動して、WiFi繋げに行くとき表示
  while (WiFi.status() != WL_CONNECTED) {//未接続なら
    vTaskDelay(500 / portTICK_PERIOD_MS);//500ms休ませ、cpu使用権を譲る。RTOS関数。(delay()は、ずっとCPUで計るから無駄。)
    Serial.print(".");                    //ひたすら.....打ち続ける
  }
  Serial.println("\n[Core 0] Wi-Fi Connected!");//接続されたらループを抜けて、表示
  Serial.print("[Core 0] IP Address: http://");//http:を表示
  Serial.println(WiFi.localIP());//IPアドレス表示

  // ルート (/) にアクセスで index_html.h を返信
  server.on("/", []() {//このサーバのルート(=トップページ)にアクセスがあったら、
    server.send(200, "text/html", INDEX_HTML);//200(正常),コンテンツタイプ「text/html」(ブラウザに対しての指示),HTML全体(ヘッダファイル丸々) を送信
  });

  // /data にアクセスで最新のJSONデータを返信
  server.on("/data", []() {//このサーバの/dataにアクセスがあったら、
    float bass, total;
    bool alert;

    // セマフォを取得して安全に読む
    xSemaphoreTake(g_data_mutex, portMAX_DELAY);//コア間共有データにアクセスするから、他のプロセスに触らせない
    bass  = g_bass_db;//共有データを持ってくる(ここから3行)
    total = g_total_db;
    alert = g_is_alert;
    xSemaphoreGive(g_data_mutex);//データアクセス終わったから、いじってよくする

    //json形式に文字列を組み立てる {"bass":bassの値,"total":totalの値"alert":alertの値}　値は、上で共有データ引っ張ってきたのをそのまま貼り付けただけ
    String json = "{\"bass\":" + String(bass, 1) + 
                  ",\"total\":" + String(total, 1) + 
                  ",\"alert\":" + (alert ? "true" : "false") + "}";
    server.send(200, "application/json", json);//200(正常),コンテンツタイプ「application/json」,上で組み立てたjson を送信
  });
  server.begin();//接続要求受付開始

  while (1) {
    server.handleClient();//アクセスとリクエストがないか監視
    vTaskDelay(10 / portTICK_PERIOD_MS);//10msCPU開放->隙間を生んでスムーズに動かす。CPU100%にして、安全装置で強制終了させない.
  }
}
