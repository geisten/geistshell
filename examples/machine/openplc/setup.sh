#!/bin/sh
# Bring up a real IEC 61131-3 PLC runtime and point geistshell at it.
#
# Why this exists: heater.py is a machine I wrote, so rules written against it
# would be graded by their own author. OpenPLC is a foreign runtime with a
# foreign Modbus stack and a foreign address map — the first thing in this
# repo that can disagree with my assumptions.
#
# Modbus is published on 5502, not 502: binding below 1024 needs root, and
# needing root to run a test is how a test stops being run.
set -eu

IMAGE=openplc:v3
SRC=${OPENPLC_SRC:-/tmp/OpenPLC_v3}
WEB=${OPENPLC_WEB:-8080}
MODBUS=${OPENPLC_MODBUS:-5502}

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    [ -d "$SRC" ] || git clone --depth 1 https://github.com/thiagoralves/OpenPLC_v3.git "$SRC"
    docker build -t "$IMAGE" "$SRC"
fi

docker rm -f openplc >/dev/null 2>&1 || true
docker run -d --name openplc -p "$WEB:8080" -p "$MODBUS:502" "$IMAGE" >/dev/null

C=$(mktemp)
until curl -s -o /dev/null http://127.0.0.1:"$WEB"/login; do sleep 2; done
curl -s -c "$C" -o /dev/null -X POST http://127.0.0.1:"$WEB"/login \
    -d "username=openplc&password=openplc"

# The Modbus server only listens once a program is compiled and the runtime is
# started — an open port is not an answering PLC, which is what the first probe
# from geistshell reported as SPG_E_IO.
curl -s -b "$C" "http://127.0.0.1:$WEB/compile-program?file=blank_program.st" >/dev/null
until curl -s -b "$C" http://127.0.0.1:"$WEB"/compilation-logs | grep -qiE "finished|error"; do
    sleep 2
done
curl -s -b "$C" -o /dev/null http://127.0.0.1:"$WEB"/start_plc
rm -f "$C"

# geistshell spricht kein Modbus mehr — ein Kanal ist ein Programm. Für einen
# Modbus-Endpunkt heißt das: ein Wrapper um ein vorhandenes Werkzeug (mbpoll),
# einmal geschrieben, nie in diesem Repo gepflegt.
mkdir -p build/openplc
cat >build/openplc/qw0 <<WRAP
#!/bin/sh
# %QW0 auf dem OpenPLC-Runtime: ohne Argument lesen, mit Argument schreiben.
if [ \$# -eq 0 ]; then
    mbpoll -0 -1 -q -p $MODBUS -a 1 -r 0 -t 4 127.0.0.1 | awk '/\[0\]:/ {print \$2}'
else
    mbpoll -0 -1 -q -p $MODBUS -a 1 -r 0 -t 4 127.0.0.1 -- "\$1" >/dev/null
fi
WRAP
chmod 0755 build/openplc/qw0
cat >build/openplc/plant.spg <<CFG
(device
  (channel (name "qw0") (program "$PWD/build/openplc/qw0")
           (range 0 1000) (safe 0)))
CFG

cat <<EOF
OpenPLC läuft.  Web http://127.0.0.1:$WEB (openplc/openplc), Modbus $MODBUS

  geistshell device --config build/openplc/plant.spg read qw0

(braucht mbpoll: brew install mbpoll / apt install mbpoll)

Adressabbildung (aus webserver/core/modbus.cpp, nicht aus einem Blogpost):
  %QW0..1023   Holding-Register 0..1023      FC3/FC6  -> erreichbar
  %MW0..1023   Holding-Register 1024..2047   FC3/FC6  -> erreichbar
  %IW0..1023   INPUT-Register               FC4      -> noch nicht implementiert
  %IX / %QX    Discrete Inputs / Coils      FC2/1/5  -> noch nicht implementiert
EOF
