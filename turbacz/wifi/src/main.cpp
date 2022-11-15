#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <credentials.h>
#include <cstddef>
#include <cstdlib>
#include <cmath>
#include <string>

#define NBLIND 8
#define uint unsigned int

// MQTT Broker
const char *mqtt_broker = "192.168.3.10";
const int mqtt_port = 1883;
const char *mqttUser = "blinds-wifi";

WiFiClient espClient;
PubSubClient client(espClient);

char out_buff[5] = {0, 0, 0, 0, 0};
char in_buff[5] = {0, 0, 0, 0, 0};
int cmd_ptr;

void ser_cmd(int in_byte)
{
	int b;
	if ((in_byte >= 'a') & (in_byte <= ('a' + NBLIND)))
	{
		in_buff[0] = in_byte;
		cmd_ptr = 1;
	}
	else if ((in_byte >= '0') & (in_byte <= '9') & (cmd_ptr > 0) & (cmd_ptr < 4))
	{
		in_buff[cmd_ptr] = in_byte;
		cmd_ptr++;
	}
	if (cmd_ptr > 3)
	{
		if ((in_buff[0] >= 'a') & (in_buff[0] <= ('a' + NBLIND)))
		{
			in_buff[4] = 0;
			int new_pos = atoi(in_buff + 1);
			b = in_buff[0] - 'a';
			std::string message = 'r' + std::to_string(b + 1) + ' ' + std::to_string(new_pos);
			client.publish("/blind/pos", &message[0]);
		}
		cmd_ptr = 0;
		for (uint i = 0; i < sizeof(in_buff); i++)
		{
			in_buff[i] = 0;
		}
	}
}

void callback(char *topic, uint8_t *payload, int length)
{
	Serial.println("-----------------------");
	Serial.print("Message arrived in topic: ");
	Serial.println(topic);
	Serial.print("Message:");
	if (length == 1)
	{
		for (int i = 0; i < NBLIND; i++)
		{
			Serial.write('A' + i);
		}
	}
	else
	{
		int blind = payload[0];
		int state = 0;
		for (int i = 1; i < length; i++)
		{
			state += (payload[length - i] - '0') * pow(10, i - 1);
		}
		sprintf(out_buff, "%c%03d", blind, state);
		Serial.write(out_buff);
		Serial.flush();
	}
	Serial.println();
}

void setup()
{
	Serial.begin(115200);
	Serial.swap();
	WiFi.begin(ssid, password);
	while (WiFi.status() != WL_CONNECTED)
	{
		delay(500);
		Serial.println("Connecting to WiFi..");
	}
	client.setServer(mqtt_broker, mqtt_port);
	client.setCallback(callback);
	Serial.println(WiFi.localIP());
	while (!client.connected())
	{
		Serial.printf("\nThe client blinds-wifi connects to the public mqtt broker\n");
		if (client.connect("blinds-wifi", mqttUser, mqttPassword))
		{
			Serial.println("Public emqx mqtt broker connected");
		}
		else
		{
			Serial.print("failed with state ");
			Serial.print(client.state());
			delay(2000);
		}
	}
	client.subscribe("/blind/cmd");
}

void loop()
{
	while (Serial.available() > 0)
	{
		ser_cmd(Serial.read());
	}
	client.loop();
}