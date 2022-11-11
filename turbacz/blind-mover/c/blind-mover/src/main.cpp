#include <Arduino.h>

HardwareSerial mySerial(PB11, PB10);

char out_buff[5] = {0, 0, 0, 0, 0};
char in_buff[5] = {0, 0, 0, 0, 0};
int blinds[7] = {800, 0, 0, 0, 0, 0, 0};
int buff_index;

void move_blind(int new_pos, int blind)
{
	int motor_startup_time = 2000;
	int full_lift_time = 20000;
	delay(motor_startup_time);
	if (blinds[blind - 'a'] <= new_pos)
	{
		int minimum_difference = 1000 / (full_lift_time / 1000 * 2);
		for (int position = blinds[blind - 'a']; position <= new_pos; position += 25)
		{
			delay(500);
			sprintf(out_buff, "%c%03d", blind, position);
			mySerial.write(out_buff);
			blinds[blind - 'a'] = position;
		}
		if ((new_pos == 999) & (blinds[blind - 'a'] != 999)) {
			delay(500);
			sprintf(out_buff, "%c%03d", blind, 999);
			mySerial.write(out_buff);
			blinds[blind - 'a'] = 999;
		}
	}
	else
	{
		int minimum_difference = -1000 / (full_lift_time / 1000 * 2);
		for (int position = blinds[blind - 'a']; position >= new_pos; position -= 25)
		{
			delay(500);
			sprintf(out_buff, "%c%03d", blind, position);
			mySerial.write(out_buff);
			blinds[blind - 'a'] = position;
		}
	}
}

void get_commmand(int in_byte)
{
	int blind;
	int position;
	if (('A' <= in_byte) & (in_byte <= 'G'))
	{
		blind = in_byte + 32;
		int position = blinds[in_byte - (int)'A'];
		sprintf(out_buff, "%c%03d", blind, position);
		mySerial.write(out_buff);
	}
	else if ((in_byte >= 'a') & (in_byte <= ('g')))
	{
		in_buff[0] = (char)in_byte;
		buff_index = 1;
	}
	else if ((in_byte >= '0') & (in_byte <= '9') & (buff_index > 0) & (buff_index < 4))
	{
		in_buff[buff_index] = in_byte;
		buff_index++;
	}
	if (buff_index > 3)
	{
		if ((in_buff[0] >= 'a') & (in_buff[0] <= ('g')))
		{
			int new_pos = atoi(in_buff + 1);
			blind = in_buff[0];
			move_blind(new_pos, blind);
		}
		buff_index = 0;
		for (uint i = 0; i < sizeof(in_buff); i++)
		{
			in_buff[i] = 0;
		}
	}
}

void setup()
{
	mySerial.begin(115200);
	pinMode(PB12, OUTPUT);
	pinMode(PB13, OUTPUT);
	pinMode(PB14, OUTPUT);
	pinMode(PB15, OUTPUT);
	pinMode(PA8, OUTPUT);
	pinMode(PA9, OUTPUT);
	pinMode(PB6, OUTPUT);
	pinMode(PB7, OUTPUT);
	pinMode(PA6, OUTPUT);
	pinMode(PB7, OUTPUT);
	pinMode(PA4, OUTPUT);
	pinMode(PA5, OUTPUT);
	pinMode(PA2, OUTPUT);
	pinMode(PA3, OUTPUT);
	pinMode(PA1, OUTPUT);
	pinMode(PA0, OUTPUT);
}

void loop()
{
	if (mySerial.available() > 0)
	{
		get_commmand(mySerial.read());
	}
}
