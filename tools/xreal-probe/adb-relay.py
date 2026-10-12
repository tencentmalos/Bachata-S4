#!/usr/bin/env python3
"""Optional localhost relay when macOS blocks an existing adb server's LAN access.

Does not restart/replace the shared adb server. Stop with Ctrl-C after testing.
"""
import argparse
import asyncio

async def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=5555)
    parser.add_argument("--listen", type=int, default=15555)
    args = parser.parse_args()

    async def bridge(reader, writer):
        peer = None
        tasks = []
        async def copy(source, target):
            while chunk := await source.read(65536):
                target.write(chunk)
                await target.drain()
        try:
            remote, peer = await asyncio.wait_for(asyncio.open_connection(args.host, args.port), 5)
            tasks = [asyncio.create_task(copy(reader, peer)), asyncio.create_task(copy(remote, writer))]
            await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
        except (OSError, asyncio.TimeoutError) as error:
            print(f"relay connection failed: {error}", flush=True)
        finally:
            for task in tasks:
                task.cancel()
            if tasks:
                await asyncio.gather(*tasks, return_exceptions=True)
            for stream in (writer, peer):
                if stream:
                    stream.close()
                    try:
                        await stream.wait_closed()
                    except OSError:
                        pass

    server = await asyncio.start_server(bridge, "127.0.0.1", args.listen)
    print(f"127.0.0.1:{args.listen} -> {args.host}:{args.port}; adb connect 127.0.0.1:{args.listen}", flush=True)
    async with server:
        await server.serve_forever()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
