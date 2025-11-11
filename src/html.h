#ifndef HTML_H
#define HTML_H
const char* info_html = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>Klaussometer Info</title>
  <style>
    body {
      background-color: #f0f2f5;
      font-family: Arial, sans-serif;
      display: flex;
      justify-content: center;
      align-items: center;
      height: 100vh;
      margin: 0;
      color: #333;
    }
    .container {
      background-color: #fff;
      padding: 30px;
      border-radius: 10px;
      box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
      width: 90%;
      max-width: 400px;
    }
    h1 {
      color: #007bff;
      margin-bottom: 20px;
      text-align: center;
    }
    p {
        color: #555;
        font-size: 14px;
        text-align: left;
        margin: 5px 0;
    }
    .section-title {
        font-weight: bold;
        color: #007bff;
        margin-top: 20px;
        text-align: left;
    }
    .link-button {
        display: inline-block;
        background-color: #007bff;
        color: #fff;
        border: none;
        padding: 12px 24px;
        border-radius: 5px;
        cursor: pointer;
        font-size: 16px;
        transition: background-color 0.3s ease;
        text-decoration: none;
        margin-top: 20px;
    }
    .link-button:hover {
      background-color: #0056b3;
    }
    .link-button.danger {
        background-color: #dc3545;
    }
    .link-button.danger:hover {
        background-color: #c82333;
    }
    .data-table {
      width: 100%;
      border-collapse: collapse;
      margin-bottom: 10px;
    }
    .data-table td {
      padding: 4px 0;
      text-align: left;
      font-size: 14px;
      color: #555;
    }
    .data-table tr td:first-child {
      width: 50%;
      white-space: nowrap;
      padding-right: 10px;
    }
  </style>
  <script>
    function confirmReboot() {
      if (confirm('Are you sure you want to reboot the Klaussometer? This will interrupt all operations temporarily.')) {
        fetch('/reboot', {
          method: 'POST'
        })
        .then(response => response.text())
        .then(data => {
          alert('Rebooting... The page will reload in 15 seconds.');
          setTimeout(function() {
            window.location.reload();
          }, 15000);
        })
        .catch(error => {
          alert('Error sending reboot command: ' + error);
        });
      }
    }
  </script>
</head>
<body>
  <div class="container">
    <h1>Klaussometer Info</h1>
    {{content}}
    <a href="/logs" class="link-button">View Logs</a><br><br>
    <a href="/update" class="link-button">Update Firmware</a><br><br>
    <button onclick="confirmReboot()" class="link-button danger">Reboot Device</button>
  </div>
</body>
</html>
)=====";

const char* ota_html = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>Klaussometer OTA Update</title>
  <style>
    body {
      background-color: #f0f2f5;
      font-family: Arial, sans-serif;
      display: flex;
      justify-content: center;
      align-items: center;
      height: 100vh;
      margin: 0;
      color: #333;
    }
    .container {
      background-color: #fff;
      padding: 30px;
      border-radius: 10px;
      box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
      text-align: center;
      width: 90%;
      max-width: 400px;
    }
    h1 {
      color: #007bff;
      margin-bottom: 20px;
    }
    p {
        color: #555;
        font-size: 14px;
    }
    form {
      margin-top: 20px;
    }
    input[type="file"] {
      border: 2px dashed #ccc;
      padding: 20px;
      border-radius: 5px;
      width: calc(100% - 40px);
      margin-bottom: 20px;
    }
    input[type="submit"] {
      background-color: #007bff;
      color: #fff;
      border: none;
      padding: 12px 24px;
      border-radius: 5px;
      cursor: pointer;
      font-size: 16px;
      transition: background-color 0.3s ease;
    }
    input[type="submit"]:hover {
      background-color: #0056b3;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Klaussometer OTA Update</h1>
    <p>Current Firmware Version: {{FIRMWARE_VERSION}}</p>
    <form method="POST" action="/update" enctype="multipart/form-data">
      <input type="file" name="firmware" id="firmware" accept=".bin">
      <input type="submit" value="Update Firmware">
    </form>
  </div>
</body>
</html>
)=====";

const char* logs_html = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>Klaussometer Logs</title>
  <style>
    body {
      background-color: #f0f2f5;
      font-family: 'Courier New', monospace;
      margin: 0;
      padding: 20px;
      color: #333;
    }
    .container {
      background-color: #fff;
      padding: 20px;
      border-radius: 10px;
      box-shadow: 0 4px 8px rgba(0, 0, 0, 0.1);
      max-width: 1400px;
      margin: 0 auto;
    }
    h1 {
      color: #007bff;
      margin-bottom: 10px;
      text-align: center;
    }
    .nav-links {
      text-align: center;
      margin-bottom: 20px;
    }
    .nav-links a {
      display: inline-block;
      background-color: #007bff;
      color: #fff;
      padding: 10px 20px;
      margin: 5px;
      border-radius: 5px;
      text-decoration: none;
      transition: background-color 0.3s ease;
    }
    .nav-links a:hover {
      background-color: #0056b3;
    }
    .log-section {
      margin-bottom: 30px;
    }
    .log-section h2 {
      color: #007bff;
      border-bottom: 2px solid #007bff;
      padding-bottom: 5px;
      margin-bottom: 15px;
    }
    .log-container {
      background-color: #1e1e1e;
      color: #d4d4d4;
      padding: 15px;
      border-radius: 5px;
      max-height: 500px;
      overflow-y: auto;
      font-size: 12px;
      line-height: 1.6;
    }
    .log-entry {
      margin-bottom: 5px;
      padding: 5px 0;
      border-bottom: 1px solid #333;
      display: flex;
      align-items: flex-start;
      width: 100%;
    }
    .log-entry:last-child {
      border-bottom: none;
    }
    .log-entry.unsynced {
      background-color: rgba(255, 193, 7, 0.1);
      padding: 5px 8px;
      border-radius: 3px;
      border-left: 3px solid #ffc107;
    }
    .timestamp {
      color: #4ec9b0;
      font-weight: bold;
      margin-right: 15px;
      white-space: nowrap;
      min-width: 150px;
      flex-shrink: 0;
      text-align: left;
    }
    .log-entry.unsynced .timestamp {
      color: #ffc107;
      font-style: italic;
    }
    .message {
      color: #d4d4d4;
      flex: 1;
      min-width: 0;
      word-wrap: break-word;
      word-break: break-word;
      overflow-wrap: break-word;
      text-align: left;
    }
    .error-log .log-container {
      background-color: #2d1f1f;
    }
    .error-log .message {
      color: #f48771;
    }
    .error-log .log-entry.unsynced {
      background-color: rgba(255, 193, 7, 0.15);
    }
    .auto-refresh {
      text-align: center;
      color: #666;
      font-size: 12px;
      margin-top: 10px;
    }
    .refreshing {
      display: inline-block;
      color: #007bff;
      font-weight: bold;
      animation: pulse 1s ease-in-out infinite;
    }
    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
    .legend {
      text-align: center;
      font-size: 11px;
      color: #888;
      margin-top: 5px;
      padding: 10px;
      background-color: #f8f9fa;
      border-radius: 5px;
      margin-bottom: 20px;
    }
    .legend-item {
      display: inline-block;
      margin: 0 15px;
    }
    .legend-color {
      display: inline-block;
      width: 12px;
      height: 12px;
      margin-right: 5px;
      vertical-align: middle;
      border-radius: 2px;
    }
    .legend-color.synced {
      background-color: #4ec9b0;
    }
    .legend-color.unsynced {
      background-color: #ffc107;
    }
    .loading {
      text-align: center;
      padding: 20px;
      color: #888;
    }
  </style>
  <script>
    function formatTimestamp(timestamp, synced) {
      if (synced) {
        // Format as date/time
        const date = new Date(timestamp * 1000);
        const year = date.getFullYear();
        const month = String(date.getMonth() + 1).padStart(2, '0');
        const day = String(date.getDate()).padStart(2, '0');
        const hours = String(date.getHours()).padStart(2, '0');
        const minutes = String(date.getMinutes()).padStart(2, '0');
        const seconds = String(date.getSeconds()).padStart(2, '0');
        return `${year}-${month}-${day} ${hours}:${minutes}:${seconds}`;
      } else {
        // Format as uptime
        const seconds = timestamp;
        const days = Math.floor(seconds / (24 * 3600));
        const hours = Math.floor((seconds % (24 * 3600)) / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);
        const secs = seconds % 60;
        
        if (days > 0) {
          return `+${days}d ${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(secs).padStart(2, '0')}`;
        } else {
          return `+${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(secs).padStart(2, '0')}`;
        }
      }
    }

    function escapeHtml(text) {
      const div = document.createElement('div');
      div.textContent = text;
      return div.innerHTML;
    }

    function renderLogs(containerId, logs) {
      const container = document.getElementById(containerId);
      
      if (!logs || logs.length === 0) {
        container.innerHTML = '<div class="log-entry"><span class="message">No log entries yet</span></div>';
        return;
      }

      let html = '';
      logs.forEach(log => {
        const cssClass = log.synced ? 'log-entry' : 'log-entry unsynced';
        const timestamp = formatTimestamp(log.timestamp, log.synced);
        const message = escapeHtml(log.message);
        
        html += `<div class="${cssClass}">`;
        html += `<span class="timestamp">${timestamp}</span>`;
        html += `<span class="message">${message}</span>`;
        html += `</div>`;
      });
      
      container.innerHTML = html;
    }

    async function loadLogs() {
      const lastUpdated = document.getElementById('last-updated');
      lastUpdated.innerHTML = '<span class="refreshing">Refreshing...</span>';
      
      try {
        // Load normal logs
        const normalResponse = await fetch('/api/logs/normal');
        const normalData = await normalResponse.json();
        renderLogs('normal-logs', normalData.logs);

        // Load error logs
        const errorResponse = await fetch('/api/logs/error');
        const errorData = await errorResponse.json();
        renderLogs('error-logs', errorData.logs);
        
        // Update last refresh time
        const now = new Date();
        const timeString = now.toLocaleTimeString();
        lastUpdated.textContent = `Last updated: ${timeString}`;
      } catch (error) {
        console.error('Error loading logs:', error);
        document.getElementById('normal-logs').innerHTML = '<div class="log-entry"><span class="message">Error loading logs</span></div>';
        document.getElementById('error-logs').innerHTML = '<div class="log-entry"><span class="message">Error loading logs</span></div>';
        lastUpdated.textContent = 'Error loading logs';
      }
    }

    // Load logs when page loads
    window.addEventListener('DOMContentLoaded', function() {
      loadLogs();
      // Auto-refresh every 10 seconds
      setInterval(loadLogs, 10000);
    });
  </script>
</head>
<body>
  <div class="container">
    <h1>Klaussometer Logs</h1>
    <div class="nav-links">
      <a href="/">Info</a>
      <a href="/update">Update</a>
      <a href="/logs">Logs</a>
    </div>
    
    <div class="legend">
      <span class="legend-item">
        <span class="legend-color synced"></span>
        <span>Time synced (absolute date/time)</span>
      </span>
      <span class="legend-item">
        <span class="legend-color unsynced"></span>
        <span>Time not synced (uptime since boot)</span>
      </span>
    </div>
    
    <div class="log-section">
      <h2>Normal Logs</h2>
      <div class="log-container">
        <div id="normal-logs" class="loading">Loading logs...</div>
      </div>
    </div>
    
    <div class="log-section error-log">
      <h2>Error Logs</h2>
      <div class="log-container">
        <div id="error-logs" class="loading">Loading logs...</div>
      </div>
    </div>
    
    <div class="auto-refresh">
      <span id="last-updated">Loading...</span> • Auto-refreshes every 10 seconds
    </div>
  </div>
</body>
</html>
)=====";

#endif // HTML_H