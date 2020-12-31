#!/usr/bin/env zsh

set -e -u -x

[ -n "${NODEMCU_WIFI:-}" ] || { echo "Specify NODEMCU_WIFI"; exit 1; }
[ -n "${NODEMCU_DUT0:-}" ] || { echo "Specify NODEMCU_DUT0"; exit 1; }

: ${NODEMCU_DUT0_NUM:=int}

if [ -n "${NODEMCU_DUT1:-}" ]; then
  : ${NODEMCU_DUT1_NUM:=int}
fi

echo "Test configuration:"
echo "  DUT0 ${NODEMCU_DUT0} (${NODEMCU_DUT0_NUM})"
if [ -n "${NODEMCU_DUT1:-}" ]; then
  echo "  DUT1 ${NODEMCU_DUT1} (${NODEMCU_DUT1_NUM})"
fi

echo "Preflight..."

export TCLLIBPATH=./expectnmcu
[ -z "${NODEMCU_TESTTMP}" ] && {
  NODEMCU_TESTTMP="$(umask 077; mktemp -d -p /tmp nodemcu.XXXXXXXXXX)"
  cleanup() {
    echo rm -rf ${NODEMCU_TESTTMP}
  }
  trap cleanup EXIT
}

export NODEMCU_TESTTMP

# Bring the board up and do our IP extraction once rather than letting each
# test guess over and over.
if [ -z "${MYIP-}" ]; then
  echo "Probing for my IP address; this might take a moment..."
  MYIP=$(expect ./preflight-host-ip.expect -serial ${NODEMCU_DUT0} -wifi ${NODEMCU_WIFI})
  stty sane
  echo "Guessed my IP as ${MYIP}"
fi

./preflight-tls.sh # Make TLS keys
./preflight-lfs.sh # Make LFS images

echo "Staging LFSes and running early commands on each DUT..."

if [ -z "${NODEMCU_SKIP_LFS:-}" ]; then
  expect ./tap-driver.expect -serial ${NODEMCU_DUT0} -lfs ${NODEMCU_TESTTMP}/tmp-lfs-${NODEMCU_DUT0_NUM}.img preflight-dut.lua
  if [ -n "${NODEMCU_DUT1:-}" ]; then
    expect ./tap-driver.expect -serial ${NODEMCU_DUT1} -lfs ${NODEMCU_TESTTMP}/tmp-lfs-${NODEMCU_DUT1_NUM}.img preflight-dut.lua
  fi
fi

echo "Running on-DUT tests..."

# These are all in LFS (see preflight-lfs.sh) and so we can -noxfer and just run tests by name
expect ./tap-driver.expect -serial ${NODEMCU_DUT0} -debug -noxfer ./test-adc.lua
expect ./tap-driver.expect -serial ${NODEMCU_DUT0} -debug -noxfer ../lua_tests/mispec_file.lua
expect ./tap-driver.expect -serial ${NODEMCU_DUT0} -debug -noxfer ../lua_tests/mispec_pixbuf_1.lua
expect ./tap-driver.expect -serial ${NODEMCU_DUT0} -debug -noxfer ../lua_tests/mispec_pixbuf_2.lua

echo "Running from-host tests..."

expect ./test-net-host.expect -serial ${NODEMCU_DUT0} -wifi ${NODEMCU_WIFI} -ip ${MYIP}
expect ./test-tls.expect      -serial ${NODEMCU_DUT0} -wifi ${NODEMCU_WIFI} -ip ${MYIP}
expect ./test-mqtt.expect     -serial ${NODEMCU_DUT0} -wifi ${NODEMCU_WIFI} -ip ${MYIP}
