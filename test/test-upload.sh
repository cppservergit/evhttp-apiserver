#!/bin/bash
    
    # Your configuration
    TOKEN=$(curl --json '{"username":"mcordova", "password":"basica"}' "http://localhost:8080/login" -s | jq -r '.token')
    FILE_PATH="/home/ubuntu/comprobante.pdf"
    URL="http://localhost:8080/upload"
    
    # Verify the file exists before trying to upload
    if [ ! -f "$FILE_PATH" ]; then
        echo "Error: File $FILE_PATH not found!"
        exit 1
    fi

    echo "Packaging and uploading $FILE_PATH..."

    # 1. Base64 encode the file (-w 0 disables line wrapping)
    # 2. Pipe into jq using -R -s to read the entire base64 string from stdin 
    base64 -w 0 "$FILE_PATH" | jq -R -s \
      --arg fn "comprobante.pdf" \
      --arg ct "application/pdf" \
      --arg title "Comprobante de pago" \
      '{filename: $fn, content_type: $ct, title: $title, blob: .}' > upload_payload.json

    # 3. Send the JSON payload via curl
    curl -s -X POST "$URL" \
      -H "Authorization: Bearer $TOKEN" \
      -H "Content-Type: application/json" \
      -d @upload_payload.json | jq

    # Clean up
    rm upload_payload.json