#ifndef INDEX_HTML_H //まだ定義されてないなら
#define INDEX_HTML_H//定義する

#include <pgmspace.h>//変数データをROMに読み込み専用でおくための関数が多くあるライブラリ

//webページとして出力するために、体裁を整えて変数に突っ込むだけのファイル//PROGMEM :ROM中に読み取り専用として固定配置。 //R"rawliteral(...)rawliteral"; ""を""内にそのまま埋め込めるようにする特殊な書き方.
const char INDEX_HTML[] PROGMEM = R"rawliteral( 
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>低音騒音モニター</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body {
      background-color: #121212;
      color: #ffffff;
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
      margin: 0;
      padding: 20px;
      display: flex;
      flex-direction: column;
      align-items: center;
    }
    h1 { margin-bottom: 20px; font-size: 1.8rem; }
    
    /* 数値表示用カードパネル */
    .dashboard {
      display: flex;
      gap: 15px;
      width: 100%;
      max-width: 800px;
      margin-bottom: 20px;
    }
    .card {
      flex: 1;
      background: #1e1e1e;
      padding: 15px;
      border-radius: 12px;
      text-align: center;
      box-shadow: 0 4px 10px rgba(0,0,0,0.5);
    }
    .card-title {
      font-size: 0.9rem;
      color: #aaa;
      margin-bottom: 8px;
    }
    .card-value {
      font-size: 2.2rem;
      font-weight: bold;
    }
    .unit { font-size: 1rem; margin-left: 2px; }
    
    /* アラートバッジ */
    .status-badge {
      padding: 6px 16px;
      border-radius: 20px;
      font-weight: bold;
      display: inline-block;
      font-size: 1.1rem;
    }
    .normal { background-color: #2e7d32; color: #fff; }
    .alert { background-color: #c62828; color: #fff; animation: blink 1s infinite; }
    
    @keyframes blink {
      0% { opacity: 1; }
      50% { opacity: 0.4; }
      100% { opacity: 1; }
    }

    /* グラフコンテナ */
    .chart-container {
      width: 100%;
      max-width: 800px;
      height: 400px;
      background: #1e1e1e;
      padding: 15px;
      border-radius: 12px;
      box-shadow: 0 4px 10px rgba(0,0,0,0.5);
      box-sizing: border-box;
    }
  </style>
</head>
<body>
  <h1>低音騒音リアルタイムモニター</h1>

  <!-- 数値カード表示エリア -->
  <div class="dashboard">
    <div class="card">
      <div class="card-title">低音域 (30〜100Hz)</div>
      <div class="card-value" style="color: #00e676;"><span id="bassVal">0.0</span><span class="unit">dB</span></div>
    </div>
    <div class="card">
      <div class="card-title">全体音量</div>
      <div class="card-value" style="color: #29b6f6;"><span id="totalVal">0.0</span><span class="unit">dB</span></div>
    </div>
    <div class="card">
      <div class="card-title">ステータス</div>
      <div style="margin-top: 8px;">
        <span id="statusBadge" class="status-badge normal">正常</span>
      </div>
    </div>
  </div>

  <!-- グラフ表示エリア -->
  <div class="chart-container">
    <canvas id="noiseChart"></canvas>
  </div>

  <script>
    const ctx = document.getElementById('noiseChart').getContext('2d');
    const maxDataPoints = 30; // 画面上に表示するデータ点数

    const chart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: [],
        datasets: [
          {
            label: '低音音量 (dB)',
            data: [],
            borderColor: '#00e676',
            backgroundColor: 'rgba(0, 230, 118, 0.1)',
            fill: true,
            tension: 0.3
          },
          {
            label: '全体音量 (dB)',
            data: [],
            borderColor: '#29b6f6',
            borderDash: [5, 5],
            fill: false,
            tension: 0.3
          }
        ]
      },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: {
            ticks: { color: '#aaa' },
            grid: { color: '#333' }
          },
          y: {
            min: 0,
            // maxを固定せず自動スケール化
            title: { display: true, text: 'dB', color: '#fff' },
            ticks: { color: '#fff' },
            grid: { color: '#333' }
          }
        },
        plugins: {
          legend: { labels: { color: '#fff' } }
        }
      }
    });

    // 定期的にデータ取得 (/data)
    setInterval(() => {
      fetch('/data')
        .then(res => res.json())
        .then(data => {
          const now = new Date().toLocaleTimeString();

          // 1. 数字の更新
          document.getElementById('bassVal').innerText = data.bass.toFixed(1);
          document.getElementById('totalVal').innerText = data.total.toFixed(1);

          // 2. ステータスバッジの更新
          const badge = document.getElementById('statusBadge');
          if (data.alert) {
            badge.innerText = '騒音検知！';
            badge.className = 'status-badge alert';
          } else {
            badge.innerText = '正常';
            badge.className = 'status-badge normal';
          }

          // 3. グラフの更新
          if (chart.data.labels.length >= maxDataPoints) {
            chart.data.labels.shift();
            chart.data.datasets[0].data.shift();
            chart.data.datasets[1].data.shift();
          }

          chart.data.labels.push(now);
          chart.data.datasets[0].data.push(data.bass);
          chart.data.datasets[1].data.push(data.total);
          chart.update('none'); // アニメーションOFFでスムーズ更新
        })
        .catch(err => console.error(err));
    }, 300); // 300msごとに更新
  </script>
</body>
</html>
)rawliteral";

#endif