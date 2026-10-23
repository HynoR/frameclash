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
out=$1; shift
/build/panel/frameclash-panel --dump-png "/build/$out" "$@" >/tmp/panel.log 2>&1 || { tail -8 /tmp/panel.log; tail -3 /tmp/fcd.log; exit 1; }
