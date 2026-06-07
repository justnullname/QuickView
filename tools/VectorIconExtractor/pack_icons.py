#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pack_icons.py - Vector Icon Coordinate Packing Utility
Scales and rounds vector coordinates in GeekIconData.cpp from float to int16_t.
This reduces executable size by ~65KB when coupled with GeekIconLibrary.h changes.
"""

import os
import re
import sys

def convert_line(match):
    cmd_type = match.group(1)
    # Extract floats, remove 'f', and round them scaled by 30000
    floats = [float(x.replace('f', '')) for x in match.groups()[1:]]
    ints = [int(round(x * 30000.0)) for x in floats]
    
    # Format the new command line
    return f"{{ '{cmd_type}', {ints[0]}, {ints[1]}, {ints[2]}, {ints[3]}, {ints[4]}, {ints[5]} }}"

def run_conversion(input_path, output_path):
    if not os.path.exists(input_path):
        print(f"Error: Input file '{input_path}' not found.", file=sys.stderr)
        return False

    with open(input_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Regex matching { 'M', 0.9375f, 0.3828f, 0.0f, 0.0f, 0.0f, 0.0f }
    # Handles positive and negative numbers with decimal points and 'f' suffix
    pattern = re.compile(
        r"\{\s*'([MLBZ])'\s*,\s*([-\d\.f]+)\s*,\s*([-\d\.f]+)\s*,\s*"
        r"([-\d\.f]+)\s*,\s*([-\d\.f]+)\s*,\s*([-\d\.f]+)\s*,\s*([-\d\.f]+)\s*\}"
    )
    
    new_content = pattern.sub(convert_line, content)
    
    with open(output_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(new_content)
        
    print(f"Bake complete: Packed coordinates from '{input_path}' into '{output_path}'.")
    return True

def main():
    # Set default paths relative to script directory
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))
    
    default_input = os.path.join(repo_root, "QuickView", "GeekIconData.cpp")
    default_output = default_input

    input_file = sys.argv[1] if len(sys.argv) > 1 else default_input
    output_file = sys.argv[2] if len(sys.argv) > 2 else default_output

    print("Running icon data packing pipeline...")
    success = run_conversion(input_file, output_file)
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
