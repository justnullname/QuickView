import re

with open("QuickView/main.cpp", "r") as f:
    content = f.read()

# Find functions that handle rotation
rotation_funcs = []
lines = content.split('\n')
for i, line in enumerate(lines):
    if "Rotate" in line and "def" not in line and "{" in line:
        pass # Will use bash grep instead
