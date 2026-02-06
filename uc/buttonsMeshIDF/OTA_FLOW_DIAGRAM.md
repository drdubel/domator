# OTA Update Flow Diagram

## Simple View: Where OTA Gets Firmware

```
┌─────────────────────────────────────────────────────────────┐
│                     You Choose Where!                        │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐  │
│  │ Your Server  │    │ Cloud Storage│    │GitHub Release│  │
│  │   (Nginx)    │    │  (S3/GCS)    │    │   (Public)   │  │
│  └──────┬───────┘    └──────┬───────┘    └──────┬───────┘  │
│         │                    │                    │          │
│         └────────────────────┴────────────────────┘          │
│                              │                                │
│                    firmware.bin (HTTPS/HTTP)                 │
│                              │                                │
└──────────────────────────────┼────────────────────────────────┘
                               │
                               ▼
                    ┌────────────────────┐
                    │   MQTT Trigger     │
                    │   /switch/cmd/root │
                    │                    │
                    │  {"type": "ota",   │
                    │   "url": "https:// │
                    │    server/fw.bin"} │
                    └─────────┬──────────┘
                              │
                              ▼
                    ┌────────────────────┐
                    │    Root Node       │
                    │  (Broadcasts URL)  │
                    └─────────┬──────────┘
                              │
                ┌─────────────┼─────────────┐
                │             │             │
                ▼             ▼             ▼
        ┌──────────┐  ┌──────────┐  ┌──────────┐
        │ Switch 1 │  │ Switch 2 │  │ Relay 1  │
        │          │  │          │  │          │
        │ Downloads│  │ Downloads│  │ Downloads│
        │ from URL │  │ from URL │  │ from URL │
        └──────────┘  └──────────┘  └──────────┘
```

## Detailed Flow

```
Step 1: Build Firmware
┌─────────────────────────────────────┐
│ $ cd buttonsMeshIDF                 │
│ $ idf.py build                      │
│                                     │
│ Output: build/buttonsMeshIDF.bin   │
└─────────────────────────────────────┘
                 │
                 ▼
Step 2: Host Firmware (Choose One)
┌─────────────────────────────────────┐
│ Option A: Local Server             │
│   $ python3 -m http.server 8000    │
│   URL: http://192.168.1.100:8000/  │
│        buttonsMeshIDF.bin           │
├─────────────────────────────────────┤
│ Option B: Cloud (AWS S3)           │
│   $ aws s3 cp firmware.bin          │
│       s3://my-bucket/               │
│   URL: https://bucket.s3.aws.com/  │
│        firmware.bin                 │
├─────────────────────────────────────┤
│ Option C: GitHub Release           │
│   Upload to GitHub release v1.0.0  │
│   URL: https://github.com/user/    │
│        repo/releases/download/      │
│        v1.0.0/firmware.bin          │
└─────────────────────────────────────┘
                 │
                 ▼
Step 3: Trigger OTA via MQTT
┌─────────────────────────────────────┐
│ $ mosquitto_pub \                   │
│     -t "/switch/cmd/root" \         │
│     -m '{                            │
│       "type": "ota_trigger",        │
│       "url": "https://server/fw.bin"│
│     }'                               │
└─────────────────────────────────────┘
                 │
                 ▼
Step 4: Root Receives & Broadcasts
┌─────────────────────────────────────┐
│  Root Node (ESP32)                  │
│  ┌────────────────────────────────┐ │
│  │ 1. Parse MQTT message          │ │
│  │ 2. Extract URL                 │ │
│  │ 3. Create MSG_TYPE_OTA_TRIGGER │ │
│  │ 4. Broadcast to mesh network   │ │
│  └────────────────────────────────┘ │
└─────────────────────────────────────┘
                 │
     ┌───────────┼───────────┐
     │           │           │
     ▼           ▼           ▼
Step 5: All Nodes Download
┌───────────┐ ┌───────────┐ ┌───────────┐
│ Switch 1  │ │ Switch 2  │ │ Relay 1   │
│           │ │           │ │           │
│ ┌───────┐ │ │ ┌───────┐ │ │ ┌───────┐ │
│ │LED→   │ │ │ │LED→   │ │ │ │LED→   │ │
│ │Blue   │ │ │ │Blue   │ │ │ │Blue   │ │
│ └───────┘ │ │ └───────┘ │ │ └───────┘ │
│           │ │           │ │           │
│ Downloads │ │ Downloads │ │ Downloads │
│ HTTPS →   │ │ HTTPS →   │ │ HTTPS →   │
│ firmware  │ │ firmware  │ │ firmware  │
└───────────┘ └───────────┘ └───────────┘
                 │
                 ▼
Step 6: Install & Restart
┌─────────────────────────────────────┐
│ Each device:                        │
│ 1. Verifies firmware signature      │
│ 2. Writes to OTA partition          │
│ 3. Sets boot partition              │
│ 4. Restarts device                  │
│ 5. Boots with new firmware          │
└─────────────────────────────────────┘
```

## Network Architecture

```
Internet/Cloud
     │
     │ HTTPS (443)
     │
     ▼
┌─────────────────────────────────────────────────┐
│         Firmware Server (You Control)           │
│  ┌──────────────────────────────────────────┐  │
│  │  /firmware/                               │  │
│  │    ├── buttonsMeshIDF-v1.0.0.bin         │  │
│  │    ├── buttonsMeshIDF-v1.0.1.bin         │  │
│  │    └── buttonsMeshIDF-v1.0.2.bin         │  │
│  └──────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
                     ▲
                     │ HTTPS GET request
                     │ (Downloads firmware)
                     │
┌────────────────────┴─────────────────────────────┐
│           Local Network (WiFi Mesh)               │
│                                                   │
│  ┌─────────────┐         ┌─────────────┐        │
│  │  MQTT       │ MQTT    │   Root      │        │
│  │  Broker     │◄────────┤   Node      │        │
│  │             │  1883   │             │        │
│  └─────────────┘         └──────┬──────┘        │
│                                 │                 │
│                          Mesh Network            │
│                       (ESP-WIFI-MESH)            │
│                                 │                 │
│           ┌─────────────────────┼─────────────┐  │
│           │                     │             │  │
│      ┌────▼───┐           ┌────▼───┐   ┌────▼───┐
│      │ Switch │           │ Switch │   │ Relay  │
│      │ Node 1 │           │ Node 2 │   │ Node 1 │
│      └────────┘           └────────┘   └────────┘
│           │                     │             │  │
│           └─────────────────────┴─────────────┘  │
│                     All download from            │
│                     same firmware URL            │
└──────────────────────────────────────────────────┘
```

## Security Flow (HTTPS)

```
┌──────────────────────────────────────────────────┐
│                Firmware Server                    │
│  ┌────────────────────────────────────────────┐  │
│  │  SSL/TLS Certificate                       │  │
│  │  - Signed by trusted CA                    │  │
│  │  - Valid domain name                       │  │
│  │  - Not expired                             │  │
│  └────────────────────────────────────────────┘  │
└──────────────────────┬───────────────────────────┘
                       │ HTTPS (Encrypted)
                       │
                  ┌────▼─────┐
                  │   ESP32  │
                  │  Device  │
                  ├──────────┤
                  │ Verifies:│
                  │ 1. Cert  │
                  │ 2. Domain│
                  │ 3. Chain │
                  └──────────┘
                       │
                  Downloads
                  firmware.bin
                       │
                  ┌────▼─────┐
                  │ Verify   │
                  │ SHA256   │
                  │ Checksum │
                  └──────────┘
                       │
                  ┌────▼─────┐
                  │ Install  │
                  │ to OTA   │
                  │ Partition│
                  └──────────┘
```

## File Locations Reference

```
Your Computer:
  buttonsMeshIDF/
    └── build/
        └── buttonsMeshIDF.bin  ← Build output

Your Server (Choose location):
  
  Option 1: Linux Server
    /var/www/firmware/
      └── buttonsMeshIDF.bin
    
  Option 2: AWS S3
    s3://my-firmware-bucket/
      └── buttonsMeshIDF.bin
    
  Option 3: GitHub
    github.com/user/repo/releases/
      └── v1.0.0/
          └── buttonsMeshIDF.bin

ESP32 Device:
  /dev/flash
    ├── ota_0 (1408KB) ← Currently running
    ├── ota_1 (1408KB) ← Downloads here
    └── nvs (20KB)     ← Config storage
  
  After download:
    ├── ota_0 (1408KB) ← Old version
    ├── ota_1 (1408KB) ← New version (active)
    └── nvs (20KB)     ← Config preserved
```

## Quick Decision Tree

```
Need to host firmware?
    │
    ├─ Production deployment?
    │   │
    │   ├─ Yes → Use HTTPS server
    │   │        (Nginx/Apache + SSL)
    │   │        or Cloud (S3 + CloudFront)
    │   │
    │   └─ No → Development/Testing?
    │            │
    │            ├─ Yes → Python HTTP server
    │            │        python3 -m http.server
    │            │
    │            └─ Demo/Open Source?
    │                    → GitHub Releases
    │                      (Free, HTTPS included)
```

## Summary

**Answer: OTA gets firmware from wherever you tell it to!**

You specify the URL in the MQTT message:
```json
{"type": "ota_trigger", "url": "https://YOUR-SERVER/firmware.bin"}
```

The ESP32 downloads directly from that URL using HTTPS.

**See OTA_FIRMWARE_HOSTING.md for complete setup guides.**
