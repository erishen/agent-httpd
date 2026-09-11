#!/bin/bash

echo "Content-Type: text/html"
echo ""

cat << 'EOF'
<!DOCTYPE html>
<html>
<head>
    <title>Form Handler</title>
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
            border-bottom: 2px solid #2196F3;
            padding-bottom: 10px;
        }
        .form-group {
            margin-bottom: 15px;
        }
        label {
            display: block;
            margin-bottom: 5px;
            font-weight: bold;
            color: #555;
        }
        input[type="text"], input[type="email"] {
            width: 100%;
            padding: 10px;
            border: 1px solid #ddd;
            border-radius: 5px;
            font-size: 16px;
            box-sizing: border-box;
        }
        button {
            background-color: #4CAF50;
            color: white;
            padding: 12px 24px;
            border: none;
            border-radius: 5px;
            cursor: pointer;
            font-size: 16px;
        }
        button:hover {
            background-color: #45a049;
        }
        .result {
            margin-top: 20px;
            padding: 15px;
            background-color: #e3f2fd;
            border-radius: 5px;
            border-left: 4px solid #2196F3;
        }
    </style>
</head>
<body>
    <div class="container">
        <h1>CGI Form Handler</h1>
        <form action="/cgi-bin/form.cgi" method="POST">
            <div class="form-group">
                <label for="name">Name:</label>
                <input type="text" id="name" name="name" placeholder="Enter your name">
            </div>
            <div class="form-group">
                <label for="email">Email:</label>
                <input type="email" id="email" name="email" placeholder="Enter your email">
            </div>
            <div class="form-group">
                <label for="message">Message:</label>
                <input type="text" id="message" name="message" placeholder="Enter your message">
            </div>
            <button type="submit">Submit</button>
        </form>
EOF

FORM_DATA=""

if [ "$REQUEST_METHOD" = "POST" ]; then
    FORM_DATA="$(cat)"
elif [ -n "$QUERY_STRING" ]; then
    FORM_DATA="$QUERY_STRING"
fi

if [ -n "$FORM_DATA" ]; then
    echo '<div class="result">'
    echo "<h3>Submitted Data (via $REQUEST_METHOD):</h3>"
    echo '<ul>'
    
    IFS='&' read -ra PARAMS <<< "$FORM_DATA"
    for param in "${PARAMS[@]}"; do
        key=$(echo "$param" | cut -d'=' -f1)
        value=$(echo "$param" | cut -d'=' -f2-)
        value=$(echo "$value" | sed 's/+/ /g')
        value=$(printf '%b' "$(printf '%s' "$value" | sed 's/%/\\x/g')")
        # Escape HTML metacharacters before echoing user data (XSS guard)
        value=$(printf '%s' "$value" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g')
        key=$(printf '%s' "$key" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g')
        echo "<li><strong>$key:</strong> $value</li>"
    done
    
    echo '</ul>'
    echo '</div>'
fi

cat << 'EOF'
    </div>
</body>
</html>
EOF