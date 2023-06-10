# Turbacz

Stable version of project **Domator**.

### Installation

1. Install python poetry module with ```pip install poetry```

2. Then copy this repository with ```git clone https://github.com/drdubel/domator```

3. Enter ```turbacz``` directory and run ```poetry install```

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

2. Create file ```.env``` in ```turbacz/turbacz/data``` directory and insert there:

~~~
GOOGLE_CLIENT_ID=YOUR_CLIENT_ID
GOOGLE_CLIENT_SECRET=YOUR_CLIENT_SECRET
~~~

3. Create ```authorized.py``` file in ```turbacz/turbacz/data``` directory with:

~~~
authorized = [YOUR_AUTHORIZED_EMAILS]
~~~

4. Run ```cookies_reset.py``` file from turbacz directory

5. Create ```secrets.py``` file in ```turbacz/turbacz/data``` directory with:  

~~~
mqtt_password = "YOUR_MQTT_PASSWORD"
~~~

6. Run Webapp with ```poetry run uvicorn turbacz.main:app --log-level debug --port 8000 --reload``` from ```turbacz``` directory

7. Open Turbacz on http://127.0.0.1:8000
#### It should work!
