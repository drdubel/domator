# Quick Start Guide - MQTT Setup

## The Problem You're Seeing

```
E (56871) NODE_ROOT: MQTT error
W (56874) NODE_ROOT: MQTT disconnected
E (76880) esp-tls: [sock=48] select() timeout
E (76881) transport_base: Failed to open a new connection: 32774
```

**What this means:** Your root node is trying to connect to an MQTT broker at `mqtt://192.168.1.100:1883`, but there's no broker running there.

## Solution: 5-Minute Setup

### Step 1: Install Mosquitto (MQTT Broker)

**Linux/Raspberry Pi:**
```bash
sudo apt-get update
sudo apt-get install mosquitto mosquitto-clients
sudo systemctl start mosquitto
```

**Mac:**
```bash
brew install mosquitto
brew services start mosquitto
```

**Windows:**
- Download from https://mosquitto.org/download/
- Install and run as service

### Step 2: Find Your Computer's IP Address

**Linux/Mac:**
```bash
ifconfig | grep "inet " | grep -v 127.0.0.1
# Example output: inet 192.168.1.150
```

**Windows:**
```cmd
ipconfig
# Look for IPv4 Address: 192.168.1.150
```

### Step 3: Configure Mosquitto for Network Access

By default, Mosquitto only accepts connections from localhost. Allow network connections:

**Edit config:**
```bash
sudo nano /etc/mosquitto/mosquitto.conf
```

**Add these lines:**
```ini
listener 1883 0.0.0.0
allow_anonymous true
```

**Restart:**
```bash
sudo systemctl restart mosquitto
```

### Step 4: Update ESP32 Firmware Configuration

```bash
cd /path/to/domator/uc/buttonsMeshIDF
idf.py menuconfig
```

Navigate to:
```
→ Domator Mesh Configuration
  → MQTT Broker URL
```

Change from `mqtt://192.168.1.100` to `mqtt://YOUR_IP` (e.g., `mqtt://192.168.1.150`)

Press `S` to save, then `Q` to quit.

### Step 5: Rebuild and Flash

```bash
idf.py build
idf.py flash monitor
```

### Step 6: Verify Success

**ESP32 logs should show:**
```
I (xxxx) IP_EVENT: Root got IP: 192.168.1.25
I (xxxx) NODE_ROOT: Initializing MQTT client
I (xxxx) NODE_ROOT: MQTT connected          ← Success!
I (xxxx) NODE_ROOT: MQTT subscribed
```

**No more errors like:**
```
E (xxxx) esp-tls: [sock=48] select() timeout  ← Gone!
E (xxxx) NODE_ROOT: MQTT error                ← Gone!
```

## Test MQTT Communication

### Subscribe to All Topics

```bash
mosquitto_sub -h YOUR_IP -t "#" -v
```

### Send a Test Command

```bash
# Toggle relay 'a' on all relays
mosquitto_pub -h YOUR_IP -t "/relay/cmd" -m "a"

# Turn ON relay 'a' on specific device
mosquitto_pub -h YOUR_IP -t "/relay/cmd/1074207536" -m "a1"
```

### Expected Output

When you send a command, you should see:
1. **Mosquitto logs** showing connection from ESP32
2. **ESP32 logs** showing MQTT message received
3. **Relay action** (relay toggles/changes state)
4. **MQTT status update** published back

## Understanding Root Election

### Which Device Becomes Root?

**Answer:** The device with the **strongest WiFi signal** to your router.

With your setup (2 ESP32-C3 + 1 ESP32 relay):
```
Device with strongest signal → Becomes ROOT
  ↓
Gets IP via DHCP
  ↓
Connects to MQTT broker
  ↓
Other 2 devices become LEAF nodes (mesh only)
```

### Identify Root Node

Check serial logs:
- **Root:** Shows "Root got IP" and "MQTT connected"
- **Leaf:** Shows "Parent connected - Layer: 2"

## Common Issues

### Issue 1: Still Getting MQTT Errors

**Check:**
1. Is Mosquitto running?
   ```bash
   sudo systemctl status mosquitto
   ```

2. Can you reach broker from ESP32's network?
   ```bash
   mosquitto_pub -h YOUR_IP -t test -m "hello"
   ```

3. Is firewall blocking port 1883?
   ```bash
   sudo ufw allow 1883/tcp  # Linux
   ```

### Issue 2: Connection Refused

**Likely cause:** Mosquitto only listening on localhost.

**Fix:** Add to `/etc/mosquitto/mosquitto.conf`:
```ini
listener 1883 0.0.0.0
allow_anonymous true
```

Then restart:
```bash
sudo systemctl restart mosquitto
```

### Issue 3: Wrong Device is Root

**This is normal** - the device with the best WiFi signal becomes root.

**It doesn't matter** - any device can be root, all work the same.

**To influence:** Move desired root device closer to WiFi router.

### Issue 4: Multiple Devices Show MQTT Errors

**Check:** Only ONE device should show "MQTT connected".

**If multiple try to connect:**
- Check they have the same WiFi SSID/password
- Verify mesh configuration is identical
- See [ROOT_ELECTION.md](ROOT_ELECTION.md)

## Network Diagram

```
┌──────────────┐
│ WiFi Router  │
│ 2.4GHz       │
└──────┬───────┘
       │
   ┌───┴────────────┬───────────────┐
   │                │               │
┌──┴──┐      ┌──────┴─────┐   ┌────┴────┐
│MQTT │      │ Root Node  │   │ Leaf 1  │
│Broker│◄────►│(ESP32-C3)  │◄─►│(ESP32-C3)│
│192.x│      │192.168.1.25│   │ (mesh)  │
└─────┘      └──────┬─────┘   └─────────┘
                    │
              ┌─────┴──────┐
              │  Leaf 2    │
              │ (ESP32)    │
              │  (mesh)    │
              └────────────┘
```

## Next Steps

Once MQTT is working:

1. **Test button presses** - Press button on switch, see MQTT message
2. **Control relays** - Send MQTT command, relay responds
3. **Monitor status** - All devices report status every 15s
4. **Configure routing** - Set up button → relay mappings

## Full Documentation

- **MQTT Details:** [MQTT_TROUBLESHOOTING.md](MQTT_TROUBLESHOOTING.md)
- **Root Election:** [ROOT_ELECTION.md](ROOT_ELECTION.md)
- **Configuration:** [PHASE5_6_GUIDE.md](PHASE5_6_GUIDE.md)
- **Quick Reference:** [QUICKSTART.md](QUICKSTART.md)

## Summary

**What you need:**
1. ✅ Mosquitto installed and running
2. ✅ Mosquitto configured for network access
3. ✅ ESP32 firmware configured with your broker's IP
4. ✅ Rebuild and flash firmware

**What you get:**
- ✅ No more MQTT errors
- ✅ Root node connects successfully
- ✅ Can control devices via MQTT
- ✅ Status updates published to MQTT

**Time to setup:** 5-10 minutes

---

*Need help? Check [MQTT_TROUBLESHOOTING.md](MQTT_TROUBLESHOOTING.md) for detailed guide*
