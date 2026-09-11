#!/bin/bash

echo "Content-Type: text/html"
echo ""

cat << EOF
<!DOCTYPE html>
<html>
<head>
    <title>Hello CGI</title>
    <style>
        body {
            font-family: Arial, sans-serif;
            max-width: 800px;
            margin: 0 auto;
            padding: 20px;
            background-color: #f5f5f5;
        }
        .container {
            background-color: white;
            padding: 30px;
            border-radius: 10px;
            box-shadow: 0 2px 10px rgba(0,0,0,0.1);
        }
        h1 {
            color: #333;
            border-bottom: 2px solid #4CAF50;
            padding-bottom: 10px;
        }
        .info {
            background-color: #e8f5e9;
            padding: 15px;
            border-radius: 5px;
            margin: 20px 0;
        }
        .info h3 {
            margin-top: 0;
            color: #2e7d32;
        }
        .time {
            font-size: 24px;
            color: #1976d2;
            font-weight: bold;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>Hello from CGI!</h1>
        <div class="info">
            <h3>CGI Program Information</h3>
            <p>This is a sample CGI program executed by AgentHTTPD.</p>
        </div>
        <div class="info">
            <h3>Current Time</h3>
            <p class="time">$(date '+%Y-%m-%d %H:%M:%S')</p>
        </div>
        <div class="info">
            <h3>Environment Variables</h3>
            <ul>
                <li><strong>REQUEST_METHOD:</strong> $REQUEST_METHOD</li>
                <li><strong>QUERY_STRING:</strong> $QUERY_STRING</li>
                <li><strong>CONTENT_TYPE:</strong> $CONTENT_TYPE</li>
                <li><strong>SERVER_NAME:</strong> $SERVER_NAME</li>
                <li><strong>SERVER_PORT:</strong> $SERVER_PORT</li>
                <li><strong>SCRIPT_NAME:</strong> $SCRIPT_NAME</li>
            </ul>
        </div>
    </div>
</body>
</html>
EOF