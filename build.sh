#!/bin/sh
#
# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

mkdir workspace
cd workspace
python -m venv venv
. venv/bin/activate
pip install west
west init -m git@github.com:bcmi-labs/bughopper_stm32.git .
west update
west sdk install
pip install -r zephyr/scripts/requirements.txt 
west build --pristine -b desire4eu app


