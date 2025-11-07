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
      /* text-align: center; <-- REMOVE THIS LINE */
      width: 90%;
      max-width: 400px;
    }
    h1 {
      color: #007bff;
      margin-bottom: 20px;
      text-align: center; /* <-- ADD THIS BACK to center the main title */
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
        text-align: left; /* <-- Explicitly left-align section titles */
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
    .data-table {
      width: 100%; /* Ensure table fills container width */
      border-collapse: collapse;
      margin-bottom: 10px;
    }
    .data-table td {
      padding: 4px 0;
      text-align: left; /* Ensure data is left-aligned */
      font-size: 14px;
      color: #555;
    }
    .data-table tr td:first-child {
      width: 50%; /* Fixed width for the label column */
      white-space: nowrap;
      padding-right: 10px; /* Space between label and value */
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>Klaussometer Info</h1>
    {{content}}
    <a href="/update" class="link-button">Update Firmware</a>
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

#endif // HTML_H

const char* logs_html = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>Klaussometer Logs</title>
  <meta http-equiv="refresh" content="10">
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
    }
    .log-entry:last-child {
      border-bottom: none;
    }
    /* Style for unsynced time entries */
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
    }
    /* Different color for unsynced timestamps */
    .log-entry.unsynced .timestamp {
      color: #ffc107;
      font-style: italic;
    }
    .message {
      color: #d4d4d4;
      flex: 1;
      word-wrap: break-word;
    }
    .error-log .log-container {
      background-color: #2d1f1f;
    }
    .error-log .message {
      color: #f48771;
    }
    /* Style for unsynced entries in error log */
    .error-log .log-entry.unsynced {
      background-color: rgba(255, 193, 7, 0.15);
    }
    .auto-refresh {
      text-align: center;
      color: #666;
      font-size: 12px;
      margin-top: 10px;
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
  </style>
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
        {{normal_logs}}
      </div>
    </div>
    
    <div class="log-section error-log">
      <h2>Error Logs</h2>
      <div class="log-container">
        {{error_logs}}
      </div>
    </div>
    
    <div class="auto-refresh">Page auto-refreshes every 10 seconds</div>
  </div>
</body>
</html>
)=====";