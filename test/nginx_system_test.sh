#!/bin/bash
#
# Copyright 2012 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Author: jefftk@google.com (Jeff Kaufman)
#
#
# Runs pagespeed's generic system test.  Will eventually run nginx-specific
# tests as well.
#
# Exits with status 0 if all tests pass.
# Exits with status 1 immediately if any test fails.
# Exits with status 2 if command line args are wrong.
#
# Usage:
#   ./ngx_system_test.sh primary_port secondary_port mod_pagespeed_dir
# Example:
#   ./ngx_system_test.sh 8050 8051 /path/to/mod_pagespeed
#

if [ "$#" -ne 4 ] ; then
  echo "Usage: $0 primary_port secondary_port mod_pagespeed_dir"
  echo "  nginx_executable"
  exit 2
fi

PRIMARY_PORT="$1"
SECONDARY_PORT="$2"
MOD_PAGESPEED_DIR="$3"
NGINX_EXECUTABLE="$4"

PRIMARY_HOSTNAME="localhost:$PRIMARY_PORT"
SECONDARY_HOSTNAME="localhost:$SECONDARY_PORT"

SERVER_ROOT="$MOD_PAGESPEED_DIR/src/install/"

# We need check and check_not before we source SYSTEM_TEST_FILE that provides
# them.
function handle_failure_simple() {
  echo "FAIL"
  exit 1
}
function check_simple() {
  echo "     check" "$@"
  "$@" || handle_failure_simple
}
function check_not_simple() {
  echo "     check_not" "$@"
  "$@" && handle_failure_simple
}

this_dir="$( cd $(dirname "$0") && pwd)"

# stop nginx
killall nginx

TEST_TMP="$this_dir/tmp"
rm -r "$TEST_TMP"
check_simple mkdir "$TEST_TMP"
FILE_CACHE="$TEST_TMP/file-cache/"
check_simple mkdir "$FILE_CACHE"

# set up the config file for the test
PAGESPEED_CONF="$TEST_TMP/pagespeed_test.conf"
PAGESPEED_CONF_TEMPLATE="$this_dir/pagespeed_test.conf.template"
# check for config file template
check_simple test -e "$PAGESPEED_CONF_TEMPLATE"
# create PAGESPEED_CONF by substituting on PAGESPEED_CONF_TEMPLATE
echo > $PAGESPEED_CONF <<EOF
This file is automatically generated from $PAGESPEED_CONF_TEMPLATE"
by nginx_system_test.sh; don't edit here."
EOF
cat $PAGESPEED_CONF_TEMPLATE \
  | sed 's#@@TEST_TMP@@#'"$TEST_TMP/"'#' \
  | sed 's#@@FILE_CACHE@@#'"$FILE_CACHE/"'#' \
  | sed 's#@@SERVER_ROOT@@#'"$SERVER_ROOT"'#' \
  | sed 's#@@PRIMARY_PORT@@#'"$PRIMARY_PORT"'#' \
  | sed 's#@@SECONDARY_PORT@@#'"$SECONDARY_PORT"'#' \
  >> $PAGESPEED_CONF
# make sure we substituted all the variables
check_not_simple grep @@ $PAGESPEED_CONF

# start nginx with new config
check_simple "$NGINX_EXECUTABLE" -c "$PAGESPEED_CONF"

# run generic system tests
SYSTEM_TEST_FILE="$MOD_PAGESPEED_DIR/src/install/system_test.sh"

if [ ! -e "$SYSTEM_TEST_FILE" ] ; then
  echo "Not finding $SYSTEM_TEST_FILE -- is mod_pagespeed not in a parallel"
  echo "directory to ngx_pagespeed?"
  exit 2
fi

PSA_JS_LIBRARY_URL_PREFIX="ngx_pagespeed_static"

PAGESPEED_EXPECTED_FAILURES="
  ~compression is enabled for rewritten JS.~
  ~convert_meta_tags~
  ~insert_dns_prefetch~
  ~In-place resource optimization~
"

# The existing system test takes its arguments as positional parameters, and
# wants different ones than we want, so we need to reset our positional args.
set -- "$PRIMARY_HOSTNAME"
source $SYSTEM_TEST_FILE

# nginx-specific system tests

start_test Check for correct default X-Page-Speed header format.
OUT=$($WGET_DUMP $EXAMPLE_ROOT/combine_css.html)
check_from "$OUT" egrep -q \
  '^X-Page-Speed: [0-9]+[.][0-9]+[.][0-9]+[.][0-9]+-[0-9]+'

start_test pagespeed is defaulting to more than PassThrough
fetch_until $TEST_ROOT/bot_test.html 'grep -c \.pagespeed\.' 2






# When we allow ourself to fetch a resource because the Host header tells us
# that it is one of our resources, we should be fetching it from ourself.
start_test Loopback fetches go to local IPs without DNS lookup

# If we're properly fetching from ourself we will issue loopback fetches for
# /mod_pagespeed_example/combine_javascriptN.js, which will succeed, so
# combining will work.  If we're taking 'Host:www.google.com' to mean that we
# should fetch from www.google.com then those fetches will fail because
# google.com won't have /mod_pagespeed_example/combine_javascriptN.js and so
# we'll not rewrite any resources.

URL="$HOSTNAME/mod_pagespeed_example/combine_javascript.html"
URL+="?ModPagespeed=on&ModPagespeedFilters=combine_javascript"
fetch_until "$URL" "fgrep -c .pagespeed." 1 --header=Host:www.google.com

# If this accepts the Host header and fetches from google.com it will fail with
# a 404.  Instead it should use a loopback fetch and succeed.
URL="$HOSTNAME/mod_pagespeed_example/.pagespeed.ce.8CfGBvwDhH.css"
check wget -O /dev/null --header=Host:www.google.com "$URL"

check_failures_and_exit
