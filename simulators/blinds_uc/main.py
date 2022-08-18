import asyncio
import signal

import aioconsole

# gmqtt also compatibility with uvloop
import uvloop
from gmqtt import Client as MQTTClient

asyncio.set_event_loop_policy(uvloop.EventLoopPolicy())

stan_rolet = {
    "r1": 123,
    "r2": 231,
    "r3": 312,
    "r4": 213,
    "r5": 132,
    "r6": 100,
    "r7": 129,
    "r8": 100,
    "r9": 129,
}

STOP = asyncio.Event()


async def move_blind(client, blind, pos):
    while stan_rolet[blind] != pos:
        if abs(stan_rolet[blind] - pos) > 10:
            if stan_rolet[blind] < pos:
                stan_rolet[blind] += 10
            elif stan_rolet[blind] > pos:
                stan_rolet[blind] -= 10
        else:
            if stan_rolet[blind] < pos:
                stan_rolet[blind] += 1
            elif stan_rolet[blind] > pos:
                stan_rolet[blind] -= 1
        client.publish("/blind/pos", {blind: stan_rolet[blind]})
        await asyncio.sleep(0.01)


def on_connect(client, flags, rc, properties):
    print("Connected")
    client.subscribe("/blind/cmd", qos=0)


def on_message(client, topic, payload, qos, properties):
    payload = payload.decode()
    print("Received message: ", topic, payload, qos, properties)
    if payload == "S":
        client.publish("/blind/pos", stan_rolet)
    else:
        roleta, nowa_pozycja = payload.split()
        nowa_pozycja = int(nowa_pozycja)
        asyncio.create_task(move_blind(client, roleta, nowa_pozycja))


def on_disconnect(client, packet, exc=None):
    print("Disconnected")


def on_subscribe(client, mid, qos, properties):
    print("SUBSCRIBED")


def ask_exit(*args):
    STOP.set()


async def main(broker_host):
    client = MQTTClient("client-id")

    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect
    client.on_subscribe = on_subscribe

    # client.set_auth_credentials(token, None)
    await client.connect(broker_host)

    # client.publish('TEST/TIME', str(time.time()), qos=1)
    async def handle_cmd():
        while True:
            command = await aioconsole.ainput(">")
            try:
                path, msg = command.split(None, 1)
            except:
                print(f"Wrong command: {command}")
                continue
            client.publish(path, msg)

    await asyncio.gather(STOP.wait(), handle_cmd())
    await client.disconnect()


if __name__ == "__main__":
    loop = asyncio.get_event_loop()

    loop.add_signal_handler(signal.SIGINT, ask_exit)
    loop.add_signal_handler(signal.SIGTERM, ask_exit)

    loop.run_until_complete(main("127.0.0.1"))
