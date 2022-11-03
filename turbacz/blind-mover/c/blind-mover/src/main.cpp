#include <Arduino.h>

HardwareSerial mySerial2(PB11, PB10);

char out_buff[5] = {0, 0, 0, 0, 0};
char in_buff[5] = {0, 0, 0, 0, 0};
int blinds[7] = {0, 0, 0, 0, 0, 0, 0};
int buff_index;

void

	void
	get_commmand(int in_byte)
{
	int blind;
	int position;
	if (('A' <= in_byte) & (in_byte <= 'G'))
	{
		blind = in_byte + 32;
		int position = blinds[in_byte - (int)'A'];
		sprintf(out_buff, "%c%03d", blind, position);
		mySerial2.write(out_buff);
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
			in_buff[4] = 0;
			int new_pos = atoi(in_buff + 1);
			blind = in_buff[0];
			sprintf(out_buff, "%c%03d", blind, new_pos);
			mySerial2.write(out_buff);
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
	mySerial2.begin(115200);
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
	if (mySerial2.available() > 0)
	{
		get_commmand(mySerial2.read());
	}
}
