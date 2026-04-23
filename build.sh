#!/bin/sh
#
# Copyright (c) Arduino s.r.l. and/or its affiliated companies
# SPDX-License-Identifier: Apache-2.0

python -m venv venv
. venv/bin/activate
pip install west
west init
west update
pip install -r zephyr/scripts/requirements.txt 
west build --pristine -b desire4eu

