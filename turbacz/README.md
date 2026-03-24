# Turbacz

Stable version of project **Domator**.

### Installation

1. Install uv with ```pip install uv```

2. Then copy this repository with ```git clone https://github.com/drdubel/domator```

3. Enter ```turbacz``` directory and run ```uv sync```

### Run MQTT Server

1. Install mosquitto:  
On Ubuntu:
```sudo apt install mosquitto mosquitto-clients```

2. Run ```sudo mosquitto_passwd -c /etc/mosquitto/passwd turbacz``` and insert new password for mqtt user ```turbacz```

3. Open ```/etc/mosquitto/mosquitto.conf``` and add

~~~
password_file /etc/mosquitto/passwd
allow_anonymous false
~~~

4. Run mqtt server with ```mosquitto -c /etc/mosquitto/mosquitto.conf```

### Run Webapp

1. Get Client ID and Client Secret following instructions from https://support.google.com/cloud/answer/6158849?hl=en

2. Create ```turbacz.toml``` file in the ```turbacz``` directory with:

```toml
authorized = ["YOUR_AUTHORIZED_EMAIL"]
jwt_secret = "YOUR_JWT_SECRET"
session_secret = "YOUR_SESSION_SECRET"

[mqtt]
password = "YOUR_MQTT_PASSWORD"

[oidc]
client_id = "YOUR_CLIENT_ID"
client_secret = "YOUR_CLIENT_SECRET"
```

3. Run Webapp with ```uv run turbacz``` from ```turbacz``` directory

4. Open Turbacz on http://127.0.0.1:8000
#### It should work!
