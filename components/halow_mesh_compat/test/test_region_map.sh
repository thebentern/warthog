#!/bin/sh
# Regulatory domain: the build region must reach the RADIO, not just the log.
#
# main/region.h maps WARTHOG_REGION_<XX> to the country code the Morse
# regulatory database is keyed on, and components/halow/mmhalow.c passes that
# to mmwlan_lookup_regulatory_domain(). It used to pass
# CONFIG_HALOW_COUNTRY_CODE instead -- hardcoded "US" in sdkconfig.defaults --
# so warthog-eu/-jp/-kr/-au all came up on the US channel list while logging
# their own region. An EU image transmitting on 902-928 MHz is a compliance
# problem, and nothing in the build or on the device reported it.
#
# This preprocesses the REAL header under each region flag and checks the code
# that comes out. It also fails if a region is added to platformio.ini without
# a matching entry in region.h, and if mmhalow.c stops consulting it.
set -e
CC="${CC:-cc}"
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
HDR="$ROOT/main/region.h"
MMHALOW="$ROOT/components/halow/mmhalow.c"
REGDB="$ROOT/components/halow/components/mm-iot-sdk/framework/src/mmregdb/mmregdb.c"
fail=0

check() {
    region="$1"; want="$2"
    got=$(printf '#include "region.h"\nWARTHOG_COUNTRY_CODE\n' \
          | $CC -E -P -I"$ROOT/main" -DWARTHOG_REGION_$region - 2>/dev/null \
          | tr -d ' "' | tail -1)
    if [ "$got" = "$want" ]; then
        echo "ok   WARTHOG_REGION_$region maps to country code $want"
    else
        echo "FAIL WARTHOG_REGION_$region gave '$got', expected '$want'"; fail=1
    fi
    if grep -q "\.country_code = \"$want\"" "$REGDB"; then
        echo "ok   '$want' exists in the Morse regulatory database"
    else
        echo "FAIL '$want' is NOT in mmregdb.c -- lookup would fall back"; fail=1
    fi
}

check US US
check EU EU
check JP JP
check KR KR
check AU AU

# A build with no region must not silently pick one.
if printf '#include "region.h"\n' | $CC -E -P -I"$ROOT/main" - >/dev/null 2>&1; then
    echo "FAIL region.h accepted a build with NO region flag"; fail=1
else
    echo "ok   a build with no region flag is refused at compile time"
fi

# The radio must consult the build region, not the Kconfig default.
if grep -q "mmwlan_lookup_regulatory_domain(db, WARTHOG_COUNTRY_CODE)" "$MMHALOW"; then
    echo "ok   mmhalow.c looks the domain up by the BUILD region"
else
    echo "FAIL mmhalow.c is not using WARTHOG_COUNTRY_CODE for the domain lookup"; fail=1
fi
if grep -q "^ *\.country_code = WARTHOG_COUNTRY_CODE," "$MMHALOW"; then
    echo "ok   mmhalow.c sets .country_code from the BUILD region"
else
    echo "FAIL mmhalow.c is not setting .country_code from the build region"; fail=1
fi

[ $fail -eq 0 ] && echo "\nALL TESTS PASSED" || { echo "\nFAILED"; exit 1; }
