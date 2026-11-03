#!/usr/bin/env bash
#
# mayhem/build.sh — builds, from the upstream sources, in place under /mayhem:
#   (a) three libFuzzer harnesses (+ run-once `-standalone` reproducers) with the project's sources
#       instrumented by $SANITIZER_FLAGS:  fuzz_msp_shared, fuzz_dronecan_can, fuzz_dronecan_msp
#   (b) the upstream gtest unit tests the harnesses' code lives next to, with NORMAL flags, for mayhem/test.sh
#
# INAV's own unit-test build (src/test/CMakeLists.txt) downloads googletest and generates the settings
# tables through cmake; neither works offline, so the same translation units are compiled directly here
# against the apt googletest and the upstream generator (src/utils/settings.rb). Everything lands under
# the gitignored /build/ directory or at /mayhem/<binary>.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${SRC:=/mayhem}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
M=$SRC/src/main
UT=$SRC/src/test/unit
DSDL=$SRC/lib/main/Dronecan/dsdlc_generated
BUILD=$SRC/build/mayhem
rm -rf "$BUILD"
mkdir -p "$BUILD"

INCS="-I$UT -I$M -I$DSDL/include"
EXTRA_INCS=""
DRONECAN_DEFS="-DUSE_DRONECAN -DCANARD_ENABLE_TAO_OPTION=0 -DFC_VERSION_MAJOR=10 -DFC_VERSION_MINOR=0 -DFC_VERSION_PATCH_LEVEL=0"
DSDL_NAMES="uavcan.protocol.NodeStatus uavcan.protocol.GetNodeInfo_res uavcan.protocol.GetNodeInfo_req
 uavcan.protocol.SoftwareVersion uavcan.protocol.HardwareVersion uavcan.equipment.gnss.Fix2
 uavcan.equipment.gnss.Fix uavcan.equipment.gnss.Auxiliary uavcan.equipment.power.BatteryInfo
 uavcan.equipment.gnss.RTCMStream uavcan.Timestamp uavcan.equipment.gnss.ECEFPositionVelocity
 uavcan.protocol.param.GetSet_req uavcan.protocol.param.GetSet_res uavcan.protocol.param.ExecuteOpcode_req
 uavcan.protocol.param.ExecuteOpcode_res uavcan.protocol.RestartNode_req uavcan.protocol.RestartNode_res
 uavcan.protocol.param.Value uavcan.protocol.param.NumericValue uavcan.protocol.param.Empty
 uavcan.protocol.dynamic_node_id.Allocation"
DRONECAN_SRCS="drivers/dronecan/dronecan.c drivers/dronecan/dronecan_async.c drivers/dronecan/dronecan_node_status.c
 drivers/dronecan/dronecan_dna_server.c drivers/dronecan/libcanard/canard.c"
for n in $DSDL_NAMES; do DRONECAN_SRCS="$DRONECAN_SRCS ../../lib/main/Dronecan/dsdlc_generated/src/$n.c"; done
MAVLINK_SRCS="fc/fc_mavlink.c mavlink/mavlink_command.c mavlink/mavlink_guided.c mavlink/mavlink_mission.c
 mavlink/mavlink_modes.c mavlink/mavlink_ports.c mavlink/mavlink_routing.c mavlink/mavlink_runtime.c
 mavlink/mavlink_streams.c telemetry/mavlink.c common/crc.c common/maths.c common/streambuf.c
 common/string_light.c msp/msp_serial.c mavlink/mavlink_helpers.c"

# gen_settings <dir> <defs...>: the generated settings tables (settings_generated.{c,h}) for one build
gen_settings() {
    local dir=$1; shift
    mkdir -p "$dir"
    CFLAGS="-pthread $INCS -DUNIT_TEST $*" TARGET=unit SETTINGS_CXX=g++ \
        ruby "$SRC/src/utils/settings.rb" "$M" "$M/fc/settings.yaml" -o "$dir" >/dev/null
}

# compile_srcs <dir> <compiler> <flags> <defs> <srcs...>: one object per source, in parallel
compile_srcs() {
    local dir=$1 cc=$2 flags=$3 defs=$4; shift 4
    mkdir -p "$dir"
    printf '%s\n' "$@" | xargs -P "$MAYHEM_JOBS" -I{} bash -c '
        s=$1; case "$s" in /*) f=$s;; *) f='"$M"'/$s;; esac
        o='"$dir"'/$(echo "$s" | tr "/." "__").o
        '"$cc"' '"$flags"' -c -w -pthread '"$INCS $EXTRA_INCS"' -DUNIT_TEST '"$defs"' "$f" -o "$o"' _ {}
}

objs_of() { local dir=$1; shift; for s in "$@"; do echo "$dir/$(echo "$s" | tr '/.' '__').o"; done; }

# ---------------------------------------------------------------------------------------------
# (b) the unit tests, NORMAL flags (first: nothing below changes how they are built)
# ---------------------------------------------------------------------------------------------
TEST_CFLAGS="-std=gnu11 -O0 $COVERAGE_FLAGS"
build_test() {  # build_test <name> <gen:0|1> <extra incs> <defs> <srcs...>
    local name=$1 gen=$2 incs=$3 defs=$4; shift 4
    local dir=$BUILD/test_$name srcs="$*"
    mkdir -p "$dir"
    if [ "$gen" = 1 ]; then
        gen_settings "$dir" $defs
        srcs="$srcs $dir/settings_generated.c"
    fi
    EXTRA_INCS="$incs -I$dir"
    compile_srcs "$dir" "$CC" "$TEST_CFLAGS" "$defs" $srcs
    local objs; objs=$(objs_of "$dir" $srcs)
    "$CXX" -std=gnu++17 -O0 -w -Wno-c++11-narrowing -pthread $COVERAGE_FLAGS $INCS $EXTRA_INCS -DUNIT_TEST $defs \
        "$UT/${name}.cc" $objs -lgtest -lgtest_main -o "$SRC/build/mayhem/unittest_$name"
}

build_test telemetry_msp_shared_unittest 0 "" "" telemetry/msp_shared.c common/streambuf.c
build_test fc_msp_dronecan_unittest 0 "" "-DUSE_DRONECAN" fc/fc_msp_dronecan.c common/streambuf.c
build_test canard_unittest 0 "-I$M/drivers/dronecan/libcanard" "-DUSE_DRONECAN -DCANARD_ENABLE_TAO_OPTION=0 -DCANARD_INTERNAL=" drivers/dronecan/libcanard/canard.c
build_test dronecan_application_unittest 1 "" "$DRONECAN_DEFS" $DRONECAN_SRCS
build_test mavlink_unittest 1 "-I$SRC/lib/main/MAVLink" "-DUSE_TELEMETRY -DUSE_TELEMETRY_MAVLINK -DUSE_MAVLINK_MSP_TUNNEL" $MAVLINK_SRCS

# ---------------------------------------------------------------------------------------------
# (a) sanitized harnesses
# ---------------------------------------------------------------------------------------------
RELAX=""
FUZZ_CFLAGS="-std=gnu11 -O1 $SANITIZER_FLAGS $DEBUG_FLAGS -fsanitize=fuzzer-no-link"
# Three UBSan checks are relaxed, each for exactly the harnesses that otherwise die on their first input:
#  - nonnull-attribute (DroneCAN): libcanard's canardRequestOrRespond() does memcpy(dst, NULL, 0) for every
#    zero-length request such as GetNodeInfo, so the check fires before anything else can be reached;
#  - vla-bound (msp_shared): handleMspFrame() declares `uint8_t payload[payloadIncoming]` and a header-only
#    frame makes that 0, which every MSP-over-telemetry request that fits one frame exactly hits.
#  - shift-base (both): common/streambuf.c's sbufReadU32() does `sbufReadU8() << 24` on an int, which fires on
#    any 4-byte MSP read with the top bit set.
# All three are benign in practice (zero-length memcpy / alloca, int shift); ASan and every other UBSan
# check stay on.

$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$BUILD/lsan_off.o"
$CC $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$BUILD/standalone_main.o"

# link_harness <name> <harness.c> <defs> <objs...>
link_harness() {
    local name=$1 harness=$2 defs=$3; shift 3
    local hobj=$BUILD/$name.o
    $CC $FUZZ_CFLAGS $RELAX -c -w -pthread $INCS $EXTRA_INCS -DUNIT_TEST $defs "$harness" -o "$hobj"
    $CC $FUZZ_CFLAGS $RELAX $LIB_FUZZING_ENGINE "$hobj" "$@" "$BUILD/lsan_off.o" -lm -o "$SRC/$name"
    $CC $SANITIZER_FLAGS $RELAX $DEBUG_FLAGS "$BUILD/standalone_main.o" "$hobj" "$@" "$BUILD/lsan_off.o" -lm -o "$SRC/$name-standalone"
}

EXTRA_INCS=""
MSP_SRCS="telemetry/msp_shared.c common/streambuf.c"
RELAX="-fno-sanitize=vla-bound,shift-base"
compile_srcs "$BUILD/f_msp_shared" "$CC" "$FUZZ_CFLAGS $RELAX" "" $MSP_SRCS
link_harness fuzz_msp_shared "$SRC/mayhem/harnesses/fuzz_msp_shared.c" "" $(objs_of "$BUILD/f_msp_shared" $MSP_SRCS)

FDIR=$BUILD/f_dronecan
FSRCS="$DRONECAN_SRCS fc/fc_msp_dronecan.c common/streambuf.c"
EXTRA_INCS="-I$FDIR"
gen_settings "$FDIR" $DRONECAN_DEFS
RELAX="-fno-sanitize=nonnull-attribute,shift-base"
compile_srcs "$FDIR" "$CC" "$FUZZ_CFLAGS $RELAX" "$DRONECAN_DEFS" $FSRCS "$FDIR/settings_generated.c"
FOBJS=$(objs_of "$FDIR" $FSRCS "$FDIR/settings_generated.c")
link_harness fuzz_dronecan_can "$SRC/mayhem/harnesses/fuzz_dronecan.c" "$DRONECAN_DEFS" $FOBJS
link_harness fuzz_dronecan_msp "$SRC/mayhem/harnesses/fuzz_dronecan.c" "$DRONECAN_DEFS -DFUZZ_MSP" $FOBJS
