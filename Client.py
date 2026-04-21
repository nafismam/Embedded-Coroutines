import asyncio

SERVER_ADDR = ('127.0.0.1', 8080)

async def slow_talker(client_id):
    """Connects, sends a tiny bit of data, goes to sleep, then finishes."""
    print(f"[Client {client_id} - Slow Talker] Connecting...")
    reader, writer = await asyncio.open_connection(*SERVER_ADDR)

    print(f"[Client {client_id} - Slow Talker] Sending partial request: 'GET '")
    writer.write(b"GET ")
    await writer.drain()

    print(f"[Client {client_id} - Slow Talker] Sleeping for 3 seconds (forcing C++ server to yield)...")
    await asyncio.sleep(3)

    print(f"[Client {client_id} - Slow Talker] Woke up. Sending remainder of request.")
    writer.write(b"/ HTTP/1.1\r\n\r\n")
    await writer.drain()

    data = await reader.read(1024)
    print(f"[Client {client_id} - Slow Talker] Received: {data.decode().splitlines()[0]}")
    writer.close()
    await writer.wait_closed()

async def fast_talker(client_id, delay):
    """Waits a moment for the others to stall, then connects and blasts data instantly."""
    await asyncio.sleep(delay)
    print(f"\n[Client {client_id} - Fast Talker] Connecting while others are stalled...")
    reader, writer = await asyncio.open_connection(*SERVER_ADDR)

    print(f"[Client {client_id} - Fast Talker] Sending full POST request instantly.")
    writer.write(b"POST / HTTP/1.1\r\n\r\n")
    await writer.drain()

    data = await reader.read(1024)
    print(f"[Client {client_id} - Fast Talker] Received: {data.decode().splitlines()[0]}\n")
    writer.close()
    await writer.wait_closed()

async def drip_feeder(client_id):
    """Sends a request one single byte at a time with pauses, forcing maximum C++ coroutine yields."""
    print(f"[Client {client_id} - Drip Feeder] Connecting...")
    reader, writer = await asyncio.open_connection(*SERVER_ADDR)

    request = b"GET / HTTP/1.1\r\n\r\n"
    print(f"[Client {client_id} - Drip Feeder] Sending byte-by-byte...")

    for i in range(len(request)):
        writer.write(bytes([request[i]]))
        await writer.drain()
        await asyncio.sleep(0.15) # Pause between every single byte

    print(f"[Client {client_id} - Drip Feeder] Finished dripping data.")
    data = await reader.read(1024)
    print(f"[Client {client_id} - Drip Feeder] Received: {data.decode().splitlines()[0]}")
    writer.close()
    await writer.wait_closed()

async def main():
    print("==============================================")
    print(" Starting Python Async Orchestrator")
    print("==============================================\n")

    # We delay the fast talker by 1 second so it hits the server exactly
    # when the slow talker and drip feeder are actively tying up the event loop.
    await asyncio.gather(
        slow_talker(1),
        drip_feeder(2),
        fast_talker(3, delay=1.0)
    )

    print("\n==============================================")
    print(" All clients finished successfully!")
    print("==============================================")

if __name__ == "__main__":
    asyncio.run(main())