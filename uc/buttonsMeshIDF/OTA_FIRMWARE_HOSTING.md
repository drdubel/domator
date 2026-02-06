# OTA Firmware Hosting Guide

## Overview

The OTA (Over-The-Air) update system in buttonsMeshIDF downloads firmware from a URL you specify. The firmware binary can be hosted on any web server accessible to your ESP32 devices.

**Key Point:** You provide the firmware URL in the MQTT trigger message, and devices download directly from that server.

---

## Quick Answer

**Q: Where does OTA get firmware from?**

**A:** From any HTTP or HTTPS server you specify in the OTA trigger message:

```json
{
  "type": "ota_trigger",
  "url": "https://your-server.com/path/to/firmware.bin"
}
```

The device downloads the firmware from this URL using ESP-IDF's `esp_https_ota` component.

---

## Hosting Options

### Option 1: Self-Hosted Web Server (Recommended for Production)

#### Using Nginx (Linux/macOS)

**1. Install Nginx:**
```bash
# Ubuntu/Debian
sudo apt install nginx

# macOS
brew install nginx
```

**2. Configure Nginx:**
```nginx
# /etc/nginx/sites-available/firmware
server {
    listen 443 ssl;
    server_name firmware.yourdomain.com;
    
    ssl_certificate /etc/ssl/certs/your-cert.pem;
    ssl_certificate_key /etc/ssl/private/your-key.pem;
    
    root /var/www/firmware;
    
    location / {
        autoindex on;
        add_header Access-Control-Allow-Origin *;
    }
}
```

**3. Place Firmware:**
```bash
sudo mkdir -p /var/www/firmware
sudo cp build/buttonsMeshIDF.bin /var/www/firmware/
sudo chmod 644 /var/www/firmware/buttonsMeshIDF.bin
```

**4. Restart Nginx:**
```bash
sudo systemctl restart nginx
```

**5. Use URL:**
```
https://firmware.yourdomain.com/buttonsMeshIDF.bin
```

#### Using Apache (Linux/macOS)

**1. Install Apache:**
```bash
# Ubuntu/Debian
sudo apt install apache2

# macOS
brew install httpd
```

**2. Configure Virtual Host:**
```apache
# /etc/apache2/sites-available/firmware.conf
<VirtualHost *:443>
    ServerName firmware.yourdomain.com
    DocumentRoot /var/www/firmware
    
    SSLEngine on
    SSLCertificateFile /etc/ssl/certs/your-cert.pem
    SSLCertificateKeyFile /etc/ssl/private/your-key.pem
    
    <Directory /var/www/firmware>
        Options +Indexes
        Require all granted
    </Directory>
</VirtualHost>
```

**3. Enable and Start:**
```bash
sudo a2enmod ssl
sudo a2ensite firmware
sudo systemctl restart apache2
```

#### Using Python Simple Server (Development Only)

**HTTP (Development/Testing Only):**
```bash
cd build
python3 -m http.server 8000

# URL: http://192.168.1.100:8000/buttonsMeshIDF.bin
```

**HTTPS with Self-Signed Certificate:**
```bash
# Generate certificate
openssl req -new -x509 -keyout key.pem -out cert.pem -days 365 -nodes

# Create server.py
cat > server.py << 'EOF'
import http.server
import ssl

server_address = ('0.0.0.0', 8443)
httpd = http.server.HTTPServer(server_address, http.server.SimpleHTTPRequestHandler)
httpd.socket = ssl.wrap_socket(httpd.socket,
                                server_side=True,
                                certfile='cert.pem',
                                keyfile='key.pem',
                                ssl_version=ssl.PROTOCOL_TLS)
print("Server running on https://0.0.0.0:8443")
httpd.serve_forever()
EOF

python3 server.py
```

**Note:** For self-signed certificates, you may need to disable certificate verification in the ESP32 code (not recommended for production).

---

### Option 2: Cloud Storage (Easy Setup)

#### AWS S3

**1. Create S3 Bucket:**
```bash
aws s3 mb s3://my-firmware-bucket
```

**2. Upload Firmware:**
```bash
aws s3 cp build/buttonsMeshIDF.bin s3://my-firmware-bucket/buttonsMeshIDF.bin --acl public-read
```

**3. Get URL:**
```
https://my-firmware-bucket.s3.amazonaws.com/buttonsMeshIDF.bin
```

**4. Enable HTTPS (Recommended):**
- Use CloudFront distribution for HTTPS
- Or use pre-signed URLs for private access

#### Google Cloud Storage

**1. Create Bucket:**
```bash
gsutil mb gs://my-firmware-bucket
```

**2. Upload Firmware:**
```bash
gsutil cp build/buttonsMeshIDF.bin gs://my-firmware-bucket/
gsutil acl ch -u AllUsers:R gs://my-firmware-bucket/buttonsMeshIDF.bin
```

**3. Get URL:**
```
https://storage.googleapis.com/my-firmware-bucket/buttonsMeshIDF.bin
```

#### Azure Blob Storage

**1. Create Container:**
```bash
az storage container create --name firmware --public-access blob
```

**2. Upload Firmware:**
```bash
az storage blob upload \
    --container-name firmware \
    --name buttonsMeshIDF.bin \
    --file build/buttonsMeshIDF.bin
```

**3. Get URL:**
```
https://mystorageaccount.blob.core.windows.net/firmware/buttonsMeshIDF.bin
```

#### DigitalOcean Spaces

Similar to S3, supports S3-compatible tools:
```bash
s3cmd put build/buttonsMeshIDF.bin s3://my-space/buttonsMeshIDF.bin --acl-public
```

---

### Option 3: GitHub Releases (Free, Simple)

**1. Create Release:**
- Go to your GitHub repository
- Click "Releases" → "Create a new release"
- Tag version (e.g., `v1.0.0`)
- Upload `buttonsMeshIDF.bin` as asset

**2. Get Download URL:**
```
https://github.com/username/repo/releases/download/v1.0.0/buttonsMeshIDF.bin
```

**3. Use in OTA Trigger:**
```json
{
  "type": "ota_trigger",
  "url": "https://github.com/username/repo/releases/download/v1.0.0/buttonsMeshIDF.bin"
}
```

**Advantages:**
- ✅ Free
- ✅ HTTPS included
- ✅ Version tracking
- ✅ Easy to use
- ✅ No server maintenance

**Disadvantages:**
- ❌ Public (unless private repo)
- ❌ Rate limits apply
- ❌ Requires GitHub account

---

### Option 4: Local Network Server (Testing)

For local development and testing:

**Using Docker with Nginx:**
```bash
# Create Dockerfile
cat > Dockerfile << 'EOF'
FROM nginx:alpine
COPY build /usr/share/nginx/html
EOF

# Build and run
docker build -t firmware-server .
docker run -p 8080:80 firmware-server

# URL: http://192.168.1.100:8080/buttonsMeshIDF.bin
```

**Using Node.js Express:**
```javascript
// server.js
const express = require('express');
const app = express();

app.use(express.static('build'));
app.listen(8080, () => {
    console.log('Firmware server running on port 8080');
});
```

```bash
npm install express
node server.js
```

---

## Firmware Build Process

### 1. Build Firmware

**Using ESP-IDF:**
```bash
cd /path/to/buttonsMeshIDF
idf.py build

# Binary location:
# build/buttonsMeshIDF.bin
```

**Using PlatformIO:**
```bash
cd /path/to/buttonsMeshIDF
pio run -e esp32c3

# Binary location:
# .pio/build/esp32c3/firmware.bin
```

### 2. Verify Binary

```bash
# Check size (must be < 1408KB for OTA partition)
ls -lh build/buttonsMeshIDF.bin

# Calculate SHA256 (for verification)
shasum -a 256 build/buttonsMeshIDF.bin
```

### 3. Version Firmware

**Option A: Git Tag**
```bash
git tag -a v1.0.1 -m "Version 1.0.1"
git push origin v1.0.1
```

**Option B: Version in Code**
```c
// In main file
#define FIRMWARE_VERSION "1.0.1"
```

### 4. Upload to Server

Choose your hosting option and upload the binary.

---

## Triggering OTA Update

### Via MQTT

**Publish to Topic:** `/switch/cmd/root`

**Message Format:**
```json
{
  "type": "ota_trigger",
  "url": "https://your-server.com/firmware.bin"
}
```

**Using mosquitto_pub:**
```bash
mosquitto_pub -h mqtt.example.com \
    -t "/switch/cmd/root" \
    -m '{"type":"ota_trigger","url":"https://your-server.com/firmware.bin"}'
```

**Using MQTT Explorer:**
1. Connect to broker
2. Navigate to `/switch/cmd/root`
3. Publish JSON message

**Using Python:**
```python
import paho.mqtt.client as mqtt
import json

client = mqtt.Client()
client.connect("mqtt.example.com", 1883)

message = {
    "type": "ota_trigger",
    "url": "https://your-server.com/firmware.bin"
}

client.publish("/switch/cmd/root", json.dumps(message))
```

---

## Security Considerations

### HTTPS vs HTTP

**HTTPS (Recommended):**
- ✅ Encrypted transfer
- ✅ Certificate verification
- ✅ Protection against MITM attacks
- ✅ Industry standard

**HTTP (Development Only):**
- ❌ Unencrypted
- ❌ Vulnerable to MITM
- ❌ Firmware can be intercepted/modified
- ⚠️ Use only on trusted networks

### Certificate Verification

**Production (Default):**
```c
esp_http_client_config_t config = {
    .url = url,
    .crt_bundle_attach = esp_crt_bundle_attach,  // Verify certificates
    .timeout_ms = 10000,
};
```

**Development (Skip Verification):**
```c
esp_http_client_config_t config = {
    .url = url,
    .skip_cert_common_name_check = true,
    .timeout_ms = 10000,
};
```

**Warning:** Never skip certificate verification in production!

### Firmware Signing

For additional security, consider implementing firmware signing:

```c
// Optional: Verify firmware signature before applying
esp_err_t verify_firmware_signature(const esp_partition_t* update_partition) {
    // Implement signature verification
    // Return ESP_OK if valid
}
```

### Access Control

**Option 1: IP Whitelist**
```nginx
# Nginx
location /firmware {
    allow 192.168.1.0/24;
    deny all;
}
```

**Option 2: Pre-Signed URLs (S3)**
```bash
aws s3 presign s3://bucket/firmware.bin --expires-in 3600
```

**Option 3: Authentication Token**
```c
esp_http_client_config_t config = {
    .url = url,
    .auth_type = HTTP_AUTH_TYPE_BEARER,
    .bearer_token = "your-secret-token",
};
```

---

## Advanced Configurations

### Staged Rollout

Update devices in groups to minimize risk:

```python
# Update 10% of devices first
devices_stage1 = get_devices(0.1)
for device in devices_stage1:
    send_ota_trigger(device, firmware_url)
    
# Wait and monitor
time.sleep(3600)

# If successful, update remaining 90%
if check_stage1_success():
    devices_stage2 = get_devices(0.9)
    for device in devices_stage2:
        send_ota_trigger(device, firmware_url)
```

### Version-Specific Updates

```python
def send_ota_to_outdated_devices():
    devices = get_all_devices()
    for device in devices:
        current_version = get_device_version(device)
        if version_compare(current_version, "1.0.0") < 0:
            send_ota_trigger(device, 
                "https://server.com/firmware-v1.0.0.bin")
```

### Bandwidth Management

**Limit concurrent downloads:**
```python
from threading import Semaphore

max_concurrent = 5
semaphore = Semaphore(max_concurrent)

def update_device(device):
    with semaphore:
        send_ota_trigger(device, firmware_url)
        time.sleep(60)  # Stagger updates
```

**Use CDN:**
- CloudFlare
- AWS CloudFront
- Fastly
- Reduces load on origin server
- Faster downloads for devices

---

## Troubleshooting

### Device Can't Download Firmware

**Check 1: URL Accessibility**
```bash
# From your computer
curl -I https://your-server.com/firmware.bin

# Should return: HTTP/1.1 200 OK
```

**Check 2: DNS Resolution**
```bash
nslookup your-server.com
# Verify IP address is correct
```

**Check 3: Certificate**
```bash
openssl s_client -connect your-server.com:443
# Check certificate chain
```

**Check 4: Firewall**
- Ensure port 443 (HTTPS) or 80 (HTTP) is open
- Check ESP32 can reach the server

### OTA Fails During Download

**Issue: Insufficient Heap**
```
E (xxx) HEALTH_OTA: OTA update failed: ESP_ERR_NO_MEM
```

**Solution:** Increase available heap before OTA:
- Close unnecessary tasks
- Free buffers
- Reduce queue sizes

**Issue: Timeout**
```
E (xxx) HEALTH_OTA: OTA update failed: ESP_ERR_TIMEOUT
```

**Solution:** Increase timeout:
```c
esp_http_client_config_t config = {
    .url = url,
    .timeout_ms = 30000,  // 30 seconds
};
```

**Issue: Firmware Too Large**
```
E (xxx) esp_ota: OTA image size exceeds partition size
```

**Solution:**
- Check partition size: 1408KB per partition
- Reduce firmware size:
  - Disable debug logging
  - Remove unused features
  - Enable compiler optimizations

### Certificate Errors

**Issue: Certificate Verification Failed**
```
E (xxx) esp-tls: Failed to verify peer certificate
```

**Solutions:**
1. Use valid certificate from trusted CA
2. Add custom CA certificate to bundle
3. For testing: Skip verification (not for production!)

---

## Best Practices

### 1. Always Test First

```bash
# Test on one device
mosquitto_pub -t "/switch/cmd/root" \
    -m '{"type":"ota_trigger","url":"https://server.com/test-firmware.bin"}'

# Wait for success
# Then deploy to all devices
```

### 2. Version Everything

```bash
# Tag releases
git tag v1.0.1
git push origin v1.0.1

# Name firmware with version
cp build/buttonsMeshIDF.bin firmware-v1.0.1.bin
```

### 3. Keep Old Versions

Don't delete old firmware files:
- Allows rollback if needed
- Historical reference
- Specific device targeting

### 4. Monitor Updates

```python
def monitor_ota_status():
    # Subscribe to device status
    client.subscribe("/switch/state/root")
    
    # Track firmware versions
    # Log success/failure
    # Alert on anomalies
```

### 5. Document Changes

Create release notes for each version:
```markdown
# v1.0.1 - 2026-02-06

## Changes
- Fixed gesture detection bug
- Improved OTA reliability
- Added peer health tracking

## Upgrade Notes
- No breaking changes
- Config compatible with v1.0.0
```

---

## Quick Reference

### Minimal Setup (Development)

```bash
# 1. Build firmware
cd buttonsMeshIDF
idf.py build

# 2. Start simple server
cd build
python3 -m http.server 8000

# 3. Trigger OTA
mosquitto_pub -h localhost -t "/switch/cmd/root" \
    -m '{"type":"ota_trigger","url":"http://192.168.1.100:8000/buttonsMeshIDF.bin"}'
```

### Production Setup

```bash
# 1. Build and version
idf.py build
cp build/buttonsMeshIDF.bin firmware-v1.0.1.bin

# 2. Upload to cloud
aws s3 cp firmware-v1.0.1.bin s3://my-firmware/

# 3. Get CloudFront URL
FIRMWARE_URL="https://d1234.cloudfront.net/firmware-v1.0.1.bin"

# 4. Trigger staged rollout
python3 staged_ota.py --url $FIRMWARE_URL --stage 0.1
```

---

## Summary

**Where OTA Gets Firmware:**
- You specify the URL in the MQTT trigger message
- Device downloads from that URL using HTTPS/HTTP
- Can be any web server: self-hosted, cloud, GitHub, etc.

**Recommended for Production:**
1. HTTPS server (Nginx/Apache)
2. Valid SSL certificate
3. CDN for distribution
4. Version control
5. Staged rollouts

**Recommended for Development:**
1. Python simple server (HTTP)
2. Local network
3. Quick iterations

---

*OTA Firmware Hosting Guide*  
*ESP-IDF 5.4.0 buttonsMeshIDF Project*  
*Last Updated: February 6, 2026*
