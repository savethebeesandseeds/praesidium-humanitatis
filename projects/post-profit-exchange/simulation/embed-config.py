#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed the example central configuration without changing its settings."""
import pathlib
import sys

project = pathlib.Path(__file__).resolve().parent.parent
text = (project / 'configs/exchange.cfg').read_text(encoding='utf-8')
if ')PHXCFG"' in text:
    raise SystemExit('Configuration contains reserved C++ embedding delimiter')
destination = pathlib.Path(sys.argv[1])
destination.parent.mkdir(parents=True, exist_ok=True)
template = (project / 'simulation/default_config.hpp.in').read_text(encoding='utf-8')
destination.write_text(template.replace('@PH_EXCHANGE_DEFAULTS@', text), encoding='utf-8')
