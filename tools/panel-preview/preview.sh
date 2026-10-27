#!/bin/sh
# Inside the container (see run.sh): a real frameclashd + mihomo with test.yaml, then render the panel.
set -e
export HOME=/tmp/h XDG_RUNTIME_DIR=/tmp/h/run LD_LIBRARY_PATH=/build/ovr
mkdir -p $HOME/Downloads $XDG_RUNTIME_DIR
cp /tools/test.yaml $HOME/Downloads/test.yaml
FRAMECLASH_MIHOMO=/build/mihomo /build/frameclashd >/tmp/fcd.log 2>&1 &
sleep 1
/build/frameclashd cli import-file ~/Downloads/test.yaml --name "家里订阅" >/dev/null
/build/frameclashd cli start >/dev/null
python3 -c '
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import time
class Slow(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(120 * 16384))
        self.end_headers()
        for _ in range(120):
            self.wfile.write(b"x" * 16384)
            self.wfile.flush()
            time.sleep(1)
    def log_message(self, *args):
        pass
ThreadingHTTPServer(("127.0.0.1", 18080), Slow).serve_forever()
' >/tmp/preview-http.log 2>&1 &
sleep 1
for i in 1 2 3; do
  curl --silent --show-error --limit-rate 16k --max-time 120 -x http://127.0.0.1:7890 \
    -o /dev/null http://127.0.0.1:18080/slow >>/tmp/preview-curl.log 2>&1 &
done
sleep 1
out=$1; shift
/build/panel/frameclash-panel --dump-png "/build/$out" "$@" >/tmp/panel.log 2>&1 || { tail -8 /tmp/panel.log; tail -3 /tmp/fcd.log; exit 1; }
