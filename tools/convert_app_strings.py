#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
convert_app_strings.py - Localization String Table Optimization Utility
Transforms template-heavy language structs in AppStrings.cpp into static instances
of a single LanguageTable struct, reducing template-based executable code size by ~50KB.
"""

import os
import re
import sys

def run_conversion(input_path, output_path):
    if not os.path.exists(input_path):
        print(f"Error: Input file '{input_path}' not found.", file=sys.stderr)
        return False

    with open(input_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Define the static constants that are defined once and not translated
    static_constants = {
        "Settings_Text_Copyright": 'L"\\u00A9 2025\\u2013%s Vivor Loong (GitHub: @justnullname)"',
        "Settings_Text_License": 'L"Licensed under GNU GPL v3.0"'
    }

    # 1. Find the ApplyT function body and extract the order of variables
    apply_match = re.search(r"template\s*<\s*typename\s+T\s*>\s*void\s+ApplyT\s*\(\s*\)\s*\{([^}]+)\}", content)
    if not apply_match:
        print("Error: Could not find ApplyT function in input.", file=sys.stderr)
        return False
        
    apply_body = apply_match.group(1)
    raw_variables = re.findall(r"([A-Za-z0-9_]+)\s*=\s*T::\1\s*;", apply_body)
    
    # Filter duplicates and static constants while preserving order
    seen = set()
    variables = []
    for v in raw_variables:
        if v in static_constants:
            continue
        if v not in seen:
            seen.add(v)
            variables.append(v)
            
    print(f"Extracted {len(variables)} unique variables from ApplyT (removed {len(raw_variables) - len(variables)} duplicates/constants).")
    
    # 2. Extract translation structs for each language
    struct_pattern = re.compile(r"struct\s+([A-Z]{2})\s*\{([^}]+)\};")
    structs = struct_pattern.findall(content)
    print(f"Found {len(structs)} language structs: {[s[0] for s in structs]}")
    
    lang_data = {}
    
    # Regex to match wide string literals: L"..."
    # A single literal is L"(?:[^"\\]|\\.)*"
    # The initializer is one or more of these, separated by whitespace/newlines.
    member_pattern = re.compile(
        r"static\s+constexpr\s+const\s+wchar_t\s*\*\s*([A-Za-z0-9_]+)\s*=\s*((?:\s*L\"(?:[^\"\\]|\\.)*\")+)\s*;"
    )

    for lang, body in structs:
        members = member_pattern.findall(body)
        
        member_dict = {}
        for name, val in members:
            if name in static_constants:
                continue
            member_dict[name] = val.strip()
            
        lang_data[lang] = member_dict
        print(f"  Parsed {len(member_dict)} strings for language {lang}")

    # Verify that all variables have values in all languages
    for lang, m_dict in lang_data.items():
        missing = [v for v in variables if v not in m_dict]
        if missing:
            print(f"Warning: Language {lang} is missing values for: {missing}", file=sys.stderr)
            for m in missing:
                m_dict[m] = "nullptr"

    # 3. Generate the new AppStrings.cpp content
    out = []
    out.append('#include "pch.h"')
    out.append('#include "AppStrings.h"')
    out.append('#include <windows.h> // For GetUserDefaultUILanguage')
    out.append('')
    out.append('namespace AppStrings {')
    out.append('')
    out.append('// ----------------------------------------------------------------')
    out.append('// Global Pointers (Definition)')
    out.append('// ----------------------------------------------------------------')
    for v in variables:
        out.append(f"const wchar_t *{v} = nullptr;")
    out.append('')
    out.append('// --- Static Constants ---')
    for k, v in static_constants.items():
        out.append(f"const wchar_t *{k} = {v};")
    out.append('')
    out.append('// ----------------------------------------------------------------')
    out.append('// Language Table Structure')
    out.append('// ----------------------------------------------------------------')
    out.append('struct LanguageTable {')
    for v in variables:
        out.append(f"    const wchar_t *{v};")
    out.append('};')
    out.append('')
    
    # Generate tables for each language
    for lang in [s[0] for s in structs]:
        out.append('// ----------------------------------------------------------------')
        out.append(f'// {lang} Table')
        out.append('// ----------------------------------------------------------------')
        out.append(f'static const LanguageTable Table_{lang} = {{')
        for v in variables:
            val = lang_data[lang].get(v, "nullptr")
            out.append(f"    {val}, // {v}")
        out.append('};')
        out.append('')

    # Generate the single Apply function
    out.append('// ----------------------------------------------------------------')
    out.append('// Apply Helper')
    out.append('// ----------------------------------------------------------------')
    out.append('void Apply(const LanguageTable& t) {')
    for v in variables:
        out.append(f"  {v} = t.{v};")
    out.append('}')
    out.append('')
    
    # Generate Init and SetLanguage functions
    out.append('void Init() { SetLanguage(Language::Auto); }')
    out.append('')
    out.append('void SetLanguage(Language lang) {')
    out.append('  Language target = lang;')
    out.append('  if (target == Language::Auto) {')
    out.append('    LANGID id = GetUserDefaultUILanguage();')
    out.append('    switch (PRIMARYLANGID(id)) {')
    out.append('    case LANG_CHINESE:')
    out.append('      target = Language::ChineseSimplified;')
    out.append('      break;')
    out.append('    case LANG_RUSSIAN:')
    out.append('      target = Language::Russian;')
    out.append('      break;')
    out.append('    case LANG_GERMAN:')
    out.append('      target = Language::German;')
    out.append('      break;')
    out.append('    case LANG_SPANISH:')
    out.append('      target = Language::Spanish;')
    out.append('      break;')
    out.append('    case LANG_JAPANESE:')
    out.append('      target = Language::Japanese;')
    out.append('      break;')
    out.append('    case LANG_FRENCH:')
    out.append('      target = Language::French;')
    out.append('      break;')
    out.append('    default:')
    out.append('      target = Language::English;')
    out.append('      break;')
    out.append('    }')
    out.append('  }')
    out.append('')
    out.append('  switch (target) {')
    out.append('  case Language::ChineseSimplified:')
    out.append('    Apply(Table_CN);')
    out.append('    break;')
    out.append('  case Language::ChineseTraditional:')
    out.append('    Apply(Table_TW);')
    out.append('    break;')
    out.append('  case Language::Japanese:')
    out.append('    Apply(Table_JA);')
    out.append('    break;')
    out.append('  case Language::Russian:')
    out.append('    Apply(Table_RU);')
    out.append('    break;')
    out.append('  case Language::German:')
    out.append('    Apply(Table_DE);')
    out.append('    break;')
    out.append('  case Language::Spanish:')
    out.append('    Apply(Table_ES);')
    out.append('    break;')
    out.append('  case Language::French:')
    out.append('    Apply(Table_FR);')
    out.append('    break;')
    out.append('  case Language::English:')
    out.append('  default:')
    out.append('    Apply(Table_EN);')
    out.append('    break;')
    out.append('  }')
    out.append('}')
    out.append('}')
    
    with open(output_path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    print(f"Bake complete: Optimized AppStrings into '{output_path}'.")
    return True

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, ".."))
    
    default_input = os.path.join(repo_root, "QuickView", "AppStrings.cpp")
    default_output = default_input

    input_file = sys.argv[1] if len(sys.argv) > 1 else default_input
    output_file = sys.argv[2] if len(sys.argv) > 2 else default_output

    print("Running localization optimization pipeline...")
    success = run_conversion(input_file, output_file)
    sys.exit(0 if success else 1)

if __name__ == "__main__":
    main()
