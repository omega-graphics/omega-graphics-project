#!/usr/bin/env python3
"""Generates a version.h file."""
import sys

output = sys.argv[1] if len(sys.argv) > 1 else "version.h"
with open(output, "w") as f:
    f.write('#define APP_VERSION "1.0.0"\n')

print(f"Generated {output}")
