# MQTT Connection Troubleshooting

## Overview

The MQTT connection is **only used by the root node** to communicate with external services. If you're seeing MQTT connection errors, this guide will help you diagnose and fix them.

## Understanding the Error

### Error Messages

```
E (56871) NODE_ROOT: MQTT error
W (56874) NODE_ROOT: MQTT disconnected
E (76880) esp-tls: [sock=48] select() timeout
E (76881) transport_base: Failed to open a new connection: 32774
E (76881) mqtt_client: Error transport connect
```

### What These Mean

- **esp-tls timeout**: Cannot establish TCP connection to MQTT broker
- **Error 32774**: ESP error code for connection timeout
- **Error transport connect**: MQTT client can't reach the broker
- **MQTT disconnected**: No active connection to MQTT broker

### Why This Happens

The root node is trying to connect to an MQTT broker that either:
1. **Doesn't exist** at the configured address
2. **Isn't running** or is down
3. **Isn't reachable** from the network
4. Has **wrong credentials** (username/password)

## Quick Diagnostic

### Step 1: Identify the Root Node

Check serial logs for:
```
I (xxxx) IP_EVENT: Root got IP: 192.168.1.x
I (xxxx) NODE_ROOT: Initializing MQTT client
```

Only the **root node** tries to connect to MQTT.

### Step 2: Check MQTT Configuration

The default configuration is:
```
MQTT Broker URL: mqtt://192.168.1.100
MQTT Broker Port: 1883
MQTT Username: domator
MQTT Password: domator
```

**Question:** Do you have an MQTT broker running at `192.168.1.100:1883`?

If **NO** → You need to either:
- Install and run an MQTT broker, or
- Change the configuration to point to an existing broker

## Solution Options

### Option 1: Run Local MQTT Broker (Mosquitto)

**Most common solution for home/development use.**

#### On Linux/Mac

```bash
# Install Mosquitto
sudo apt-get install mosquitto mosquitto-clients  # Debian/Ubuntu
brew install mosquitto                             # Mac

# Start Mosquitto
sudo systemctl start mosquitto    # Linux
brew services start mosquitto     # Mac

# Test it's running
mosquitto_pub -h localhost -t test -m "hello"
```

#### On Windows

```bash
# Download from https://mosquitto.org/download/
# Install and run as service

# Test it's running (from Command Prompt)
mosquitto_pub.exe -h localhost -t test -m "hello"
```

#### On Raspberry Pi

```bash
# Install
sudo apt-get update
sudo apt-get install mosquitto mosquitto-clients

# Enable and start
sudo systemctl enable mosquitto
sudo systemctl start mosquitto

# Check status
sudo systemctl status mosquitto
```

#### Configure Mosquitto for Network Access

By default, Mosquitto only listens on localhost. To allow ESP32 to connect:

```bash
# Edit config
sudo nano /etc/mosquitto/mosquitto.conf

# Add these lines:
listener 1883
allow_anonymous true

# Restart Mosquitto
sudo systemctl restart mosquitto
```

**Security Note:** `allow_anonymous true` is OK for development but not production. For production, set up proper authentication.

### Option 2: Use Cloud MQTT Broker

**Good for remote access or if you don't want to run a local broker.**

#### Free Cloud Brokers

1. **HiveMQ Cloud** (hivemq.com)
   - Free tier available
   - Web interface
   - TLS supported

2. **CloudMQTT** (cloudmqtt.com)
   - Free tier available
   - Simple setup

3. **EMQX Cloud** (emqx.com)
   - Free trial
   - Scalable

#### Configuration Example

If you sign up for HiveMQ Cloud and get:
- Host: `abc123.s1.eu.hivemq.cloud`
- Port: `8883` (TLS)
- Username: `myuser`
- Password: `mypass`

Update your firmware configuration (see "Configuration" section below).

### Option 3: Run Without MQTT (Mesh Only)

**For testing mesh network without MQTT.**

The MQTT errors won't affect mesh functionality. Leaf nodes will still work, but:
- ❌ No external control via MQTT
- ❌ No status publishing
- ✅ Mesh communication works
- ✅ Button presses work within mesh
- ✅ Relay control via mesh works

**To suppress errors**, you can modify the code to skip MQTT when it fails, but it's better to configure it properly.

## Configuration

### Via menuconfig (Before Building)

```bash
cd /path/to/buttonsMeshIDF
idf.py menuconfig

# Navigate to:
→ Domator Mesh Configuration

# Configure:
→ MQTT Broker URL: mqtt://192.168.1.100
  (Change 192.168.1.100 to your broker's IP)
  
→ MQTT Broker Port: 1883
  (1883 for non-TLS, 8883 for TLS)
  
→ MQTT Username: domator
  (Your MQTT broker username)
  
→ MQTT Password: domator
  (Your MQTT broker password)

# Save and exit (S, then Enter, then Q)

# Rebuild and flash
idf.py build flash monitor
```

### Via sdkconfig (Manual Edit)

Edit `sdkconfig`:
```ini
CONFIG_MQTT_BROKER_URL="mqtt://192.168.1.100"
CONFIG_MQTT_BROKER_PORT=1883
CONFIG_MQTT_USERNAME="domator"
CONFIG_MQTT_PASSWORD="domator"
```

Then rebuild:
```bash
idf.py build flash monitor
```

## Verification Steps

### Step 1: Verify MQTT Broker is Running

From any computer on the same network:

```bash
# Test connection
mosquitto_pub -h 192.168.1.100 -t test -m "hello"

# If this works, broker is running and reachable
```

If you get "Connection refused" or timeout:
- Broker not running
- Firewall blocking port 1883
- Wrong IP address

### Step 2: Verify ESP32 Can Reach Broker

Check that the root node has network connectivity:

**Serial logs should show:**
```
I (xxxx) IP_EVENT: Root got IP: 192.168.1.25
```

**Ping test** (from computer):
```bash
ping 192.168.1.25  # Use the IP from logs
```

If ping works, network is OK.

### Step 3: Monitor MQTT Broker Logs

On the machine running Mosquitto:

```bash
# Watch Mosquitto logs
sudo tail -f /var/log/mosquitto/mosquitto.log

# Or run in verbose mode
sudo mosquitto -v
```

**Expected when ESP32 connects:**
```
New connection from 192.168.1.25 on port 1883
New client connected from 192.168.1.25 as ESP32_XXXX
```

### Step 4: Verify Successful Connection

**ESP32 logs should show:**
```
I (xxxx) NODE_ROOT: Initializing MQTT client
I (xxxx) NODE_ROOT: MQTT connected
I (xxxx) NODE_ROOT: MQTT subscribed, msg_id=1
```

**No errors like:**
```
E (xxxx) esp-tls: [sock=48] select() timeout
E (xxxx) NODE_ROOT: MQTT error
```

## Common Problems and Solutions

### Problem 1: Broker Not Installed

**Symptom:**
```
E (76880) esp-tls: [sock=48] select() timeout
```

**Solution:**
Install Mosquitto (see Option 1 above).

### Problem 2: Wrong Broker IP Address

**Symptom:**
```
E (76881) transport_base: Failed to open a new connection: 32774
```

**Check:**
- What IP address is configured in firmware?
- What IP address is the broker actually running on?

**Find your computer's IP:**
```bash
# Linux/Mac
ifconfig | grep "inet "

# Windows
ipconfig
```

**Update firmware configuration** to match.

### Problem 3: Firewall Blocking Port 1883

**Symptom:**
Connection timeout even though broker is running.

**Solution:**

**Linux:**
```bash
sudo ufw allow 1883/tcp
```

**Windows:**
```
Windows Firewall → Inbound Rules → New Rule
→ Port → TCP → 1883 → Allow
```

**Mac:**
```bash
# Mac usually allows by default
# Check System Preferences → Security & Privacy → Firewall
```

### Problem 4: Broker Only Listening on Localhost

**Symptom:**
- `mosquitto_pub -h localhost` works
- `mosquitto_pub -h 192.168.1.100` fails

**Solution:**

Edit `/etc/mosquitto/mosquitto.conf`:
```ini
listener 1883 0.0.0.0
allow_anonymous true
```

Restart:
```bash
sudo systemctl restart mosquitto
```

### Problem 5: Wrong Username/Password

**Symptom:**
Connection succeeds but then immediately disconnects.

**Mosquitto logs show:**
```
Connection from 192.168.1.25 failed: Authentication failed
```

**Solution:**

**Option A:** Allow anonymous (development):
```ini
# /etc/mosquitto/mosquitto.conf
allow_anonymous true
```

**Option B:** Create proper user (production):
```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd domator
# Enter password: domator

# In mosquitto.conf:
allow_anonymous false
password_file /etc/mosquitto/passwd
```

### Problem 6: Using TLS But Firmware Configured for Plain

**Symptom:**
Broker uses port 8883 (TLS), firmware configured for port 1883 (plain).

**Solution:**

Update firmware configuration:
```
MQTT Broker URL: mqtts://192.168.1.100
MQTT Broker Port: 8883
```

Or configure broker for plain port 1883.

### Problem 7: Multiple Devices Trying to Connect

**Symptom:**
All three devices show MQTT errors.

**Explanation:**
- Only the **root node** should connect to MQTT
- Leaf nodes should show: "Not root node, skipping MQTT init"

**Check logs:**
- How many devices show "Root got IP"?
- Should be **only ONE**

If multiple devices become root:
- See [ROOT_ELECTION.md](ROOT_ELECTION.md)
- Check WiFi configuration is the same on all devices

## Testing MQTT Communication

### Subscribe to Topics

On a computer with mosquitto-clients:

```bash
# Monitor all switch commands
mosquitto_sub -h 192.168.1.100 -t "/switch/cmd/#" -v

# Monitor all relay commands
mosquitto_sub -h 192.168.1.100 -t "/relay/cmd/#" -v

# Monitor all status messages
mosquitto_sub -h 192.168.1.100 -t "/switch/status/#" -v
mosquitto_sub -h 192.168.1.100 -t "/relay/status/#" -v

# Monitor everything
mosquitto_sub -h 192.168.1.100 -t "#" -v
```

### Send Test Commands

```bash
# Toggle relay 'a' on all relay nodes
mosquitto_pub -h 192.168.1.100 -t "/relay/cmd" -m "a"

# Set relay 'a' ON on specific relay (device ID 1074207536)
mosquitto_pub -h 192.168.1.100 -t "/relay/cmd/1074207536" -m "a1"

# Sync all relays
mosquitto_pub -h 192.168.1.100 -t "/relay/cmd" -m "S"
```

### Expected Behavior

**When command sent:**
1. Root node receives MQTT message
2. Root logs: "MQTT data received: topic=..."
3. Root forwards to mesh nodes
4. Target device executes command
5. Target device sends confirmation back through mesh
6. Root publishes status to MQTT

**Example log flow:**
```
[Root] I (xxx) NODE_ROOT: MQTT data received: topic=/relay/cmd/1074207536, data=a1
[Root] I (xxx) NODE_ROOT: Sending command to device 1074207536 (MAC: 6C:C8:40:...)
[Relay] I (xxx) NODE_RELAY: Received command: a1
[Relay] I (xxx) NODE_RELAY: Setting relay a to state 1
[Relay] I (xxx) NODE_RELAY: Sending relay state confirmation: a1
[Root] I (xxx) NODE_ROOT: Relay state from device 1074207536: a1
[Root] I (xxx) NODE_ROOT: Published to /relay/state/1074207536/a: 1
```

## Network Diagram

```
┌─────────────────────┐
│   WiFi Router       │
│   192.168.1.1       │
└──────────┬──────────┘
           │
     ┌─────┴─────┐
     │           │
┌────┴────┐  ┌───┴─────┐
│ Root    │  │ MQTT    │
│ Node    │  │ Broker  │
│ESP32/C3 │  │192.168. │
│.1.25    │  │1.100    │
└────┬────┘  └─────────┘
     │
     │ (Mesh Network)
     │
  ┌──┴───┐
  │      │
┌─┴─┐  ┌─┴─┐
│N1 │  │N2 │
│C3 │  │ESP│
└───┘  └───┘
```

## Minimal Setup for Testing

### Option 1: No MQTT (Test Mesh Only)

1. Flash all devices with firmware
2. Power on
3. Ignore MQTT errors
4. Test button presses and relay control via mesh

**Limitation:** No external control, no status publishing.

### Option 2: Quick Mosquitto Setup

```bash
# On any Linux/Mac/RPi computer on same network

# 1. Install
sudo apt-get install mosquitto mosquitto-clients

# 2. Configure
echo "listener 1883 0.0.0.0" | sudo tee /etc/mosquitto/mosquitto.conf
echo "allow_anonymous true" | sudo tee -a /etc/mosquitto/mosquitto.conf

# 3. Start
sudo systemctl restart mosquitto

# 4. Find your IP
ip addr show | grep "inet " | grep -v 127.0.0.1

# 5. Update ESP32 firmware with this IP
idf.py menuconfig
# → Domator Mesh Configuration → MQTT Broker URL → mqtt://YOUR_IP

# 6. Rebuild and flash
idf.py build flash monitor
```

## Production Recommendations

### Security

1. **Enable authentication:**
```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd domator
```

2. **Use TLS/SSL:**
```ini
# mosquitto.conf
listener 8883
certfile /path/to/cert.pem
keyfile /path/to/key.pem
```

3. **Restrict topics** (ACL):
```ini
# mosquitto.conf
acl_file /etc/mosquitto/acls.txt
```

### Reliability

1. **Mosquitto auto-start:**
```bash
sudo systemctl enable mosquitto
```

2. **Monitor broker status:**
```bash
sudo systemctl status mosquitto
```

3. **Log rotation:**
```ini
# /etc/mosquitto/mosquitto.conf
log_dest file /var/log/mosquitto/mosquitto.log
log_type all
```

### Backup

1. **Configuration backup:**
```bash
sudo cp /etc/mosquitto/mosquitto.conf /etc/mosquitto/mosquitto.conf.backup
```

2. **Password file backup:**
```bash
sudo cp /etc/mosquitto/passwd /etc/mosquitto/passwd.backup
```

## Summary

**Key Points:**
- ✅ MQTT errors only affect the root node
- ✅ Default MQTT broker: `mqtt://192.168.1.100:1883`
- ✅ You need to install Mosquitto or use cloud broker
- ✅ Update firmware configuration to match your broker
- ✅ Only root node connects to MQTT (leaf nodes don't need it)

**Quick Fix:**
1. Install Mosquitto: `sudo apt-get install mosquitto`
2. Find your IP: `ifconfig | grep "inet "`
3. Configure firmware: `idf.py menuconfig` → Set MQTT Broker URL
4. Rebuild: `idf.py build flash monitor`

**Verification:**
- Root logs show "MQTT connected" (not "MQTT error")
- Can send commands via `mosquitto_pub`
- Can see status via `mosquitto_sub`

---

*For root election, see [ROOT_ELECTION.md](ROOT_ELECTION.md)*  
*For configuration guide, see [QUICKSTART.md](QUICKSTART.md)*  
*Last Updated: February 7, 2026*
