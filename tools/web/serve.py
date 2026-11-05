#!/usr/bin/env python3
"""serve.py - LAN-only development server for the web build (make serve).

Serves build/web (index.html / .js / .wasm / .data) with the wasm MIME type and caching disabled, so a
rebuild is picked up by reloading the page. The .data file holds the ROM data from build/roms: keep this
server on the local network and never expose it to the internet.

usage: serve.py [--port 8097] [--dir build/web] [--bind 0.0.0.0]
"""
import argparse
import functools
import http.server
import os
import socket
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      '.wasm': 'application/wasm', '.js': 'text/javascript', '.data': 'application/octet-stream'}

    def end_headers(self):
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=8097)
    ap.add_argument('--dir', default='build/web')
    ap.add_argument('--bind', default='0.0.0.0', help='address to listen on (default: all interfaces of this host)')
    args = ap.parse_args()
    if not os.path.exists(os.path.join(args.dir, 'index.html')):
        sys.exit('%s/index.html missing: run make web first' % args.dir)
    handler = functools.partial(Handler, directory=args.dir)
    srv = http.server.ThreadingHTTPServer((args.bind, args.port), handler)
    host = socket.gethostname()
    print('serving %s on http://%s:%d/ (LAN only, Ctrl-C to stop)' % (args.dir, host, args.port), flush=True)
    srv.serve_forever()


if __name__ == '__main__':
    sys.exit(main())
