#!/usr/bin/env python3
"""SSE load test.
Spawns N concurrent connections to /stream and reads events.
Usage: python load_test.py N [host] [port]
"""
import sys, asyncio, aiohttp, time

async def consume(idx, url):
    # Each client just reads and discards data
    try:
        async with aiohttp.ClientSession() as session:
            async with session.get(url) as resp:
                async for line in resp.content:
                    if idx == 0 and line.startswith(b'data:'):
                        # Print first client's first event for sanity then silence
                        print("sample event:", line.decode()[:120], '...')
                        break
    except Exception as e:
        print(f"client {idx} error: {e}")

async def main():
    if len(sys.argv) < 2:
        print("Usage: python load_test.py N [host] [port] [duration_seconds]")
        return
    n = int(sys.argv[1])
    host = sys.argv[2] if len(sys.argv) > 2 else 'localhost'
    port = sys.argv[3] if len(sys.argv) > 3 else '8080'
    duration = int(sys.argv[4]) if len(sys.argv) > 4 else 5
    url = f"http://{host}:{port}/stream"
    print(f"Parameters: clients={n}, host={host}, port={port}, duration={duration}s")
    print(f"Spawning {n} SSE clients -> {url}")
    start = time.time()
    tasks = [asyncio.create_task(consume(i, url)) for i in range(n)]
    # Let them run for specified duration
    await asyncio.sleep(duration)
    for t in tasks: t.cancel()
    elapsed = time.time() - start
    print(f"Ran {n} clients for {elapsed:.2f}s")

if __name__ == '__main__':
    asyncio.run(main())
