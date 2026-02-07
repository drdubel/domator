# MQTT Connection Status Messages

## Overview

The root node automatically publishes connection status messages to MQTT when it connects or disconnects from the MQTT broker. This enables external systems to monitor mesh network availability, detect connection issues, and integrate with home automation platforms.

## Quick Answer

**Q: Does root post message "connected" on connection?**

**A: YES!** The root node publishes a JSON message to `/status/root/connection` topic whenever it connects to or disconnects from the MQTT broker.

```bash
# Monitor connection status
mosquitto_sub -t "/status/root/connection" -v

# Output when root connects:
/status/root/connection {"status":"connected","device_id":1074205304,"timestamp":1707308070,"firmware":"48925a3","ip":"192.168.1.45","mesh_layer":1}
```

## Message Format

### Connection Message

When root node successfully connects to MQTT broker:

```json
{
  "status": "connected",
  "device_id": 1074205304,
  "timestamp": 1707308070,
  "firmware": "48925a3",
  "ip": "192.168.1.45",
  "mesh_layer": 1
}
```

**Fields:**
- `status`: Always "connected" for connection messages
- `device_id`: Unique device ID (derived from MAC address)
- `timestamp`: Unix timestamp (seconds since epoch)
- `firmware`: Git commit hash of firmware version
- `ip`: IP address assigned to root node
- `mesh_layer`: Mesh layer (1 for root)

### Disconnection Message

When root node cleanly disconnects:

```json
{
  "status": "disconnected",
  "device_id": 1074205304,
  "timestamp": 1707308170
}
```

**Fields:**
- `status`: Always "disconnected"
- `device_id`: Device ID for identification
- `timestamp`: Time of disconnection

### Last Will and Testament (LWT)

For ungraceful disconnects (crash, power loss, network failure):

```json
{
  "status": "disconnected",
  "device_id": 1074205304,
  "timestamp": 1707308070,
  "reason": "ungraceful"
}
```

**Note:** LWT is published automatically by the broker when connection is lost unexpectedly.

## Topic Structure

### Current Topics

- `/status/root/connection` - Root node connection status

### Future Expansion

Potential future status topics:
- `/status/mesh/nodes` - Mesh network node count
- `/status/mesh/topology` - Mesh network topology changes
- `/status/device/<device_id>/health` - Individual device health

## MQTT Properties

### Quality of Service (QoS)

**QoS Level: 1** (At least once delivery)
- Messages are guaranteed to be delivered at least once
- Broker acknowledges receipt
- Suitable for status monitoring

### Retain Flag

**Retain: Enabled**
- Last status message is stored by broker
- New subscribers immediately receive last known status
- Useful for checking current state without waiting

### Example Behavior

```bash
# Start monitoring AFTER root connects
mosquitto_sub -t "/status/root/connection" -v

# You'll immediately see the last retained message:
/status/root/connection {"status":"connected",...}
```

## Usage Examples

### Basic Monitoring

Monitor connection status in terminal:

```bash
mosquitto_sub -t "/status/root/connection" -v
```

### Home Assistant Integration

Add to `configuration.yaml`:

```yaml
mqtt:
  sensor:
    - name: "Mesh Network Status"
      state_topic: "/status/root/connection"
      value_template: "{{ value_json.status }}"
      device_class: "connectivity"
      
    - name: "Mesh Root IP"
      state_topic: "/status/root/connection"
      value_template: "{{ value_json.ip | default('unknown') }}"
      
    - name: "Mesh Root Device ID"
      state_topic: "/status/root/connection"
      value_template: "{{ value_json.device_id }}"
      
    - name: "Mesh Root Firmware"
      state_topic: "/status/root/connection"
      value_template: "{{ value_json.firmware | default('unknown') }}"

  binary_sensor:
    - name: "Mesh Network Online"
      state_topic: "/status/root/connection"
      value_template: "{{ 'ON' if value_json.status == 'connected' else 'OFF' }}"
      payload_on: "ON"
      payload_off: "OFF"
      device_class: "connectivity"

automation:
  - alias: "Alert on Mesh Disconnect"
    trigger:
      - platform: mqtt
        topic: "/status/root/connection"
    condition:
      - condition: template
        value_template: "{{ trigger.payload_json.status == 'disconnected' }}"
    action:
      - service: notify.mobile_app
        data:
          title: "Mesh Network Offline"
          message: "Root node {{ trigger.payload_json.device_id }} disconnected"
```

### Node-RED Flow

```json
[
    {
        "id": "mqtt_in",
        "type": "mqtt in",
        "topic": "/status/root/connection",
        "qos": "1",
        "broker": "mqtt_broker"
    },
    {
        "id": "json_parse",
        "type": "json",
        "wires": [["check_status"]]
    },
    {
        "id": "check_status",
        "type": "switch",
        "property": "payload.status",
        "rules": [
            {"t": "eq", "v": "connected"},
            {"t": "eq", "v": "disconnected"}
        ],
        "wires": [["connected_handler"], ["disconnected_handler"]]
    }
]
```

### Python Script

```python
import paho.mqtt.client as mqtt
import json
from datetime import datetime

def on_connect(client, userdata, flags, rc):
    print(f"Connected to MQTT broker with code {rc}")
    client.subscribe("/status/root/connection")

def on_message(client, userdata, msg):
    status = json.loads(msg.payload)
    
    if status['status'] == 'connected':
        print(f"✅ Mesh network ONLINE")
        print(f"   Device ID: {status['device_id']}")
        print(f"   IP: {status.get('ip', 'N/A')}")
        print(f"   Firmware: {status.get('firmware', 'N/A')}")
        print(f"   Time: {datetime.fromtimestamp(status['timestamp'])}")
    else:
        print(f"❌ Mesh network OFFLINE")
        print(f"   Device ID: {status['device_id']}")
        print(f"   Time: {datetime.fromtimestamp(status['timestamp'])}")
        if 'reason' in status:
            print(f"   Reason: {status['reason']}")

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect("192.168.1.100", 1883, 60)
client.loop_forever()
```

### Shell Script

```bash
#!/bin/bash
# Monitor mesh network status

MQTT_HOST="192.168.1.100"
MQTT_PORT="1883"

mosquitto_sub -h "$MQTT_HOST" -p "$MQTT_PORT" \
              -t "/status/root/connection" -v | \
while read -r topic message; do
    status=$(echo "$message" | jq -r '.status')
    device_id=$(echo "$message" | jq -r '.device_id')
    timestamp=$(echo "$message" | jq -r '.timestamp')
    
    if [ "$status" = "connected" ]; then
        ip=$(echo "$message" | jq -r '.ip')
        echo "[$(date -d @$timestamp)] ✅ Device $device_id CONNECTED from $ip"
    else
        echo "[$(date -d @$timestamp)] ❌ Device $device_id DISCONNECTED"
    fi
done
```

## Monitoring Dashboard

### Grafana + InfluxDB

Store status messages in InfluxDB and visualize in Grafana:

**Telegraf MQTT Consumer Configuration:**

```toml
[[inputs.mqtt_consumer]]
  servers = ["tcp://192.168.1.100:1883"]
  topics = ["/status/root/connection"]
  data_format = "json"
  json_string_fields = ["status", "firmware", "ip"]
  tag_keys = ["device_id"]
```

**Grafana Query:**

```sql
SELECT 
  last("status") as "Status",
  last("ip") as "IP Address",
  last("firmware") as "Firmware"
FROM "mqtt_consumer" 
WHERE $timeFilter
GROUP BY "device_id"
```

## Troubleshooting

### No Messages Received

**Problem:** Not seeing any status messages

**Solutions:**
1. Verify root node is connected to MQTT:
   ```bash
   # Check serial logs for "MQTT connected"
   idf.py monitor
   ```

2. Check broker is receiving messages:
   ```bash
   # On broker host, monitor all topics
   mosquitto_sub -v -t "#"
   ```

3. Verify topic subscription:
   ```bash
   # Use wildcard to catch all status topics
   mosquitto_sub -v -t "/status/#"
   ```

### Stale Retained Message

**Problem:** Receiving old "connected" message after root disconnected

**Solutions:**
1. Clear retained message:
   ```bash
   mosquitto_pub -t "/status/root/connection" -n -r
   ```

2. Wait for LWT to trigger (up to broker keepalive timeout)

3. Restart root node to publish fresh status

### LWT Not Working

**Problem:** No disconnection message on power loss

**Checklist:**
- ✅ LWT is configured in mqtt_init()
- ✅ Broker supports LWT (Mosquitto 1.4+)
- ✅ QoS is 1 or higher
- ✅ Allow time for broker to detect disconnect (keepalive period)

### Multiple Root Nodes

**Problem:** Seeing status from different device IDs

**Explanation:** This is normal if:
- Root election changed (different device became root)
- Multiple mesh networks on same broker
- Testing with multiple devices

**Solution:** Use device_id in topic:
```
/status/root/<device_id>/connection
```

## Technical Implementation

### Code Location

File: `src/node_root.c`

**Key Functions:**
- `publish_connection_status(bool connected)` - Publishes status message
- `mqtt_event_handler()` - Handles MQTT events
- `mqtt_init()` - Configures MQTT client with LWT

### Timing

**Connection Status:**
- Published immediately after `MQTT_EVENT_CONNECTED` event
- Triggered when TCP connection established and CONNACK received
- Typically 100-500ms after network connection

**Disconnection Status:**
- Published on clean disconnect (ESP_MQTT_EVENT_DISCONNECTED)
- LWT published by broker on ungraceful disconnect
- LWT delay: broker keepalive timeout (default 120s)

### Dependencies

**Required Components:**
- cJSON library (JSON generation)
- esp_mqtt_client (MQTT client)
- esp_netif (IP address retrieval)
- esp_timer (timestamp generation)

## Best Practices

### Monitoring

1. **Always use retain flag** - Get last status immediately
2. **QoS 1 minimum** - Ensure status delivery
3. **Check LWT works** - Test ungraceful disconnect scenarios
4. **Monitor broker logs** - Verify message delivery

### Integration

1. **Handle missing fields** - IP may not be present on disconnect
2. **Use device_id** - Track which device is root
3. **Check timestamp** - Detect stale messages
4. **Implement alerts** - Notify on unexpected disconnects

### Security

1. **Use TLS** - Encrypt MQTT traffic in production
2. **Authenticate** - Require MQTT credentials
3. **Restrict topics** - Limit write access to status topics
4. **Validate JSON** - Parse with error handling

## Future Enhancements

Potential improvements:
- [ ] Add mesh node count to status
- [ ] Include memory usage statistics
- [ ] Report WiFi signal strength
- [ ] Add uptime counter
- [ ] Include mesh topology summary
- [ ] Support multiple status subscribers
- [ ] Add periodic heartbeat (every 60s)

## See Also

- [MQTT_QUICKSTART.md](MQTT_QUICKSTART.md) - Quick MQTT setup
- [MQTT_TROUBLESHOOTING.md](MQTT_TROUBLESHOOTING.md) - MQTT issues
- [ROOT_ELECTION.md](ROOT_ELECTION.md) - How root node is selected
- [DEVICE_TARGETING.md](DEVICE_TARGETING.md) - Device-specific commands
