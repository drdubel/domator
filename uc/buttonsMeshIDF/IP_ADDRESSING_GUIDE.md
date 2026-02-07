# IP Addressing Guide for ESP-WIFI-MESH

## Quick Answer to "What does this mean?"

If you see logs like:
```
I (7245) esp_netif_handlers: sta ip: 192.168.4.2, mask: 255.255.255.0, gw: 192.168.4.1
I (7245) MESH_INIT: Root got IP: 192.168.4.2
```

**This means:** Your ESP32 is **NOT connected to your home WiFi router**. The mesh is operating in **isolated mode** using its own internal network.

**Why:** WiFi credentials (`WIFI_SSID` and `WIFI_PASSWORD`) are not configured or incorrect.

**Impact:** MQTT won't work because it can't reach the broker on your home network.

**Solution:** Configure WiFi credentials (see below).

---

## Understanding the Log Messages

### Log Breakdown

```
I (6268) MESH_INIT: Root address: 6C:C8:40:07:12:79
```
- Device with MAC 6C:C8:40:07:12:79 became the mesh root node
- Any device can be root (automatic election based on WiFi signal)

```
I (7245) esp_netif_handlers: sta ip: 192.168.4.2, mask: 255.255.255.0, gw: 192.168.4.1
```
- **IP address:** 192.168.4.2 (the root's IP)
- **Subnet mask:** 255.255.255.0 (standard /24 network)
- **Gateway:** 192.168.4.1 (where packets go for external routing)

```
I (7245) MESH_INIT: Root got IP: 192.168.4.2
```
- Confirmation that root obtained an IP address
- Ready to start MQTT client

```
I (7246) NODE_ROOT: Initializing MQTT client
I (7251) NODE_ROOT: MQTT client started
```
- MQTT client started
- Will attempt to connect to broker
- **If on 192.168.4.x network, this will fail!**

---

## What Different IP Ranges Mean

### 192.168.4.x Network (⚠️ PROBLEMATIC)

**What it means:**
- ESP-WIFI-MESH internal network
- **NOT connected to external WiFi router**
- Mesh operates in isolated mode
- Uses internal DHCP server at 192.168.4.1

**Characteristics:**
- Gateway: 192.168.4.1 (mesh internal router)
- IP range: 192.168.4.2 - 192.168.4.254
- Root can communicate with leaf nodes via mesh
- **Cannot reach devices on home network**
- **MQTT broker unreachable (if on home network)**

**When this is normal:**
- Intentionally running mesh without external network
- Testing mesh functionality in isolation
- No WiFi credentials configured (by design)

**When this is a problem:**
- Need MQTT connectivity to home network broker
- Need to control devices from home automation system
- WiFi credentials should be configured but aren't

### 192.168.1.x or 192.168.0.x Network (✅ GOOD)

**What it means:**
- Connected to home WiFi router
- Root has access to local network
- Can reach MQTT broker, internet, other devices

**Characteristics:**
- Gateway: Usually 192.168.1.1 or 192.168.0.1 (your router)
- IP range: Assigned by your router's DHCP
- Full network connectivity
- MQTT can connect successfully

**Example logs:**
```
I (7245) esp_netif_handlers: sta ip: 192.168.1.45, mask: 255.255.255.0, gw: 192.168.1.1
I (7245) MESH_INIT: Root got IP: 192.168.1.45
I (7246) NODE_ROOT: Initializing MQTT client
I (7251) NODE_ROOT: MQTT client started
I (8123) NODE_ROOT: MQTT connected  ← Success!
```

### 10.x.x.x or Other Networks (✅ GOOD)

Corporate or custom networks also work fine, as long as:
- Gateway provides routing to MQTT broker
- Firewall allows port 1883 (or configured port)
- DNS works (if using hostnames)

---

## Identifying Your Scenario

### Scenario 1: WiFi Not Configured (Most Common)

**Symptoms:**
```
sta ip: 192.168.4.2, gw: 192.168.4.1
MQTT error (repeated)
```

**Root cause:**
- `WIFI_SSID` and `WIFI_PASSWORD` are empty
- Device can't connect to external router
- Mesh falls back to isolated mode

**Solution:**
Configure WiFi credentials (see Configuration Guide below)

### Scenario 2: Wrong WiFi Credentials

**Symptoms:**
```
W (5000) wifi: password or ssid is wrong
sta ip: 192.168.4.2, gw: 192.168.4.1
```

**Root cause:**
- SSID or password incorrect
- Device attempts connection but fails
- Falls back to isolated mode

**Solution:**
1. Verify SSID (case-sensitive!)
2. Verify password (check for typos)
3. Reconfigure and reflash

### Scenario 3: Router Out of Range

**Symptoms:**
```
W (10000) wifi: no AP found
sta ip: 192.168.4.2, gw: 192.168.4.1
```

**Root cause:**
- No device has good signal to router
- All devices timeout trying to connect
- Mesh operates independently

**Solution:**
1. Move devices closer to router
2. Improve WiFi signal (repeater, better antenna)
3. Check router 2.4GHz is enabled

### Scenario 4: Router 2.4GHz Disabled

**Symptoms:**
```
W (10000) wifi: no AP found
sta ip: 192.168.4.2, gw: 192.168.4.1
```

**Root cause:**
- ESP32 only supports 2.4GHz
- Router's 2.4GHz band disabled or hidden
- Only 5GHz available

**Solution:**
1. Enable 2.4GHz band on router
2. Unhide SSID if hidden
3. Check channel (ESP32 supports 1-13)

---

## Configuration Guide

### Step 1: Configure WiFi Credentials

#### Using menuconfig (ESP-IDF):

```bash
cd /path/to/buttonsMeshIDF
idf.py menuconfig
```

Navigate to:
```
Component config → Domator Mesh Configuration
```

Set:
- **WiFi SSID:** Your network name (case-sensitive)
- **WiFi Password:** Your network password

Save and exit (S, then Enter, then Q)

#### Using PlatformIO:

Edit `sdkconfig.defaults`:
```ini
CONFIG_WIFI_SSID="YourNetworkName"
CONFIG_WIFI_PASSWORD="YourPassword"
```

### Step 2: Rebuild and Flash

```bash
# ESP-IDF
idf.py build flash monitor

# PlatformIO
pio run -t upload -t monitor
```

### Step 3: Verify Connection

Expected logs after fix:
```
I (7245) esp_netif_handlers: sta ip: 192.168.1.45, mask: 255.255.255.0, gw: 192.168.1.1
                                          ↑ Your home network range
I (7245) MESH_INIT: Root got IP: 192.168.1.45
I (7246) NODE_ROOT: Initializing MQTT client
I (7251) NODE_ROOT: MQTT client started
I (8123) NODE_ROOT: MQTT connected  ← Success!
```

Key indicators:
- ✅ IP in your home network range (192.168.1.x or 192.168.0.x)
- ✅ Gateway is your router (192.168.1.1 or 192.168.0.1)
- ✅ "MQTT connected" message appears

---

## Testing and Verification

### Check 1: IP Address Range

```bash
# Monitor serial output
idf.py monitor

# Look for:
esp_netif_handlers: sta ip: X.X.X.X, gw: Y.Y.Y.Y
```

**If 192.168.4.x:**
- ❌ Not connected to home WiFi
- Fix WiFi configuration

**If 192.168.1.x or 192.168.0.x:**
- ✅ Connected successfully
- Gateway should match your router

### Check 2: MQTT Connection

```bash
# Watch for MQTT messages
idf.py monitor | grep MQTT
```

**Success:**
```
NODE_ROOT: MQTT client started
NODE_ROOT: MQTT connected
```

**Failure:**
```
NODE_ROOT: MQTT error
esp-tls: [sock=48] select() timeout
transport_base: Failed to open a new connection
```

### Check 3: Network Reachability

From root device (if you can run commands):

```bash
# Ping your router
ping 192.168.1.1

# Ping MQTT broker
ping 192.168.1.100

# Test MQTT port
nc -zv 192.168.1.100 1883
```

### Check 4: MQTT Broker Logs

On your MQTT broker:

```bash
# Mosquitto logs
sudo tail -f /var/log/mosquitto/mosquitto.log

# Look for connection from ESP32
```

Expected:
```
New connection from 192.168.1.45
Client esp32_1074205304 connected
```

---

## Advanced Diagnostics

### Get Current IP Configuration

Add debug code to `mesh_init.c`:

```c
esp_netif_ip_info_t ip_info;
esp_netif_get_ip_info(netif_sta, &ip_info);

ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&ip_info.ip));
ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info.netmask));
ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
```

### Check WiFi Configuration

Add debug code to `mesh_init.c`:

```c
wifi_config_t wifi_config;
esp_wifi_get_config(WIFI_IF_STA, &wifi_config);

ESP_LOGI(TAG, "Configured SSID: %s", wifi_config.sta.ssid);
ESP_LOGI(TAG, "Password length: %d", strlen((char*)wifi_config.sta.password));
```

### Monitor Network Traffic

Use Wireshark on your network:
1. Filter: `ip.src == 192.168.1.45` (your ESP32 IP)
2. Look for MQTT packets (port 1883)
3. Check for DHCP requests/responses
4. Verify ARP resolution

### Check Router DHCP Leases

On your router's admin page:
1. Look for device with ESP32 MAC address
2. Verify it has an IP lease
3. Check lease expiration
4. Ensure DHCP pool not exhausted

---

## Troubleshooting Common Issues

### Issue: Still Getting 192.168.4.x After Configuration

**Possible causes:**
1. **Configuration not saved**
   - Re-run menuconfig, verify settings saved
   - Check `sdkconfig` file for WiFi settings

2. **Flash not updated**
   - Reflash: `idf.py -p /dev/ttyUSB0 flash`
   - Erase flash first: `idf.py erase-flash`

3. **Wrong partition flashed**
   - ESP32 has two OTA partitions
   - Make sure flashing correct partition

4. **NVS has old config**
   - Erase NVS: `idf.py erase-flash`
   - Or programmatically: `nvs_flash_erase()`

### Issue: Connected but MQTT Still Fails

**Check:**
1. **Broker IP correct?**
   - Verify in menuconfig: MQTT Broker URL
   - Should match broker's actual IP

2. **Firewall blocking?**
   - Test from another device: `telnet 192.168.1.100 1883`
   - Check broker firewall rules

3. **Broker authentication?**
   - Configure username/password if required
   - Check broker logs for auth failures

### Issue: IP Address Keeps Changing

**Normal behavior:**
- Root election can change between devices
- Different device = different IP (unless using static)

**Solution:**
- Use DHCP reservations on router
- Or configure static IP in code

### Issue: One Device Gets IP, Others Don't

**This is normal!**
- Only root node gets external IP
- Leaf nodes communicate through mesh
- Leaf nodes have internal mesh addresses only

---

## Best Practices

### 1. Configure WiFi Before Deployment

Always set WiFi credentials before deploying devices:
```bash
idf.py menuconfig → Configure WiFi → Save → Build → Flash
```

### 2. Use DHCP Reservations

On your router:
- Reserve IPs for ESP32 devices by MAC address
- Makes devices predictable and easier to manage

### 3. Document Your Network

Keep record of:
- WiFi SSID and password
- Router IP and subnet
- MQTT broker IP
- ESP32 MAC addresses
- Assigned IP addresses

### 4. Test in Isolation First

Before connecting to home network:
1. Test mesh formation (192.168.4.x is fine for this)
2. Verify button/relay functionality
3. Then configure WiFi and test MQTT

### 5. Monitor During Deployment

Watch serial logs during first boot:
- Verify IP in correct range
- Confirm MQTT connection
- Test basic commands

---

## Network Architecture

### Isolated Mode (192.168.4.x)

```
┌──────────────────────────────────────┐
│         ESP-WIFI-MESH Network        │
│                                      │
│  ┌──────┐   ┌──────┐   ┌──────┐   │
│  │ESP32 │───│ESP32 │───│ESP32 │   │
│  │Leaf  │   │Root  │   │Leaf  │   │
│  └──────┘   └──────┘   └──────┘   │
│               │                     │
│               │ Internal Router     │
│               ↓ 192.168.4.1        │
│          (No external access)       │
└──────────────────────────────────────┘
```

### Connected Mode (192.168.1.x)

```
        Internet
            ↕
    ┌───────────────┐
    │  WiFi Router  │  192.168.1.1
    │   (Gateway)   │
    └───────┬───────┘
            │
  ┌─────────┼─────────────────────┐
  │         ↓                     │
  │  ┌────────────┐      ┌──────────────┐
  │  │ MQTT Broker│      │ESP32 Root    │
  │  │ 192.168.1  │      │192.168.1.45  │
  │  │    .100    │←─────│(mesh root)   │
  │  └────────────┘      └───────┬──────┘
  │                              ↓
  │                    ┌──────────────────┐
  │                    │  Mesh Network    │
  │                    │  ┌────┐  ┌────┐ │
  │                    │  │Leaf│  │Leaf│ │
  │                    │  └────┘  └────┘ │
  │                    └──────────────────┘
  └────────────────────────────────────────┘
           Home Network (192.168.1.x)
```

---

## Summary

**Quick Diagnosis:**

| Symptom | Meaning | Action |
|---------|---------|--------|
| IP: 192.168.4.2, GW: 192.168.4.1 | Isolated mode | Configure WiFi |
| IP: 192.168.1.x, GW: 192.168.1.1 | Connected | Check MQTT broker |
| MQTT connected | Working | ✅ All good |
| MQTT error (repeated) | Can't reach broker | Check network/config |

**Quick Fix for 192.168.4.x:**
```bash
idf.py menuconfig → Configure WiFi SSID/Password → Save
idf.py build flash monitor
# Should now see 192.168.1.x with "MQTT connected"
```

**For more help, see:**
- MQTT_TROUBLESHOOTING.md - MQTT connection issues
- ROOT_ELECTION.md - Understanding root node election
- MESH_TROUBLESHOOTING.md - General mesh issues
- README.md - Overall project documentation
