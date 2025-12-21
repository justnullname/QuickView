// stdafx.h : include file for standard system include files,
//  or project specific include files that are used frequently, but
//      are changed infrequently
//

#pragma once

#define WINVER		0x0601
#define _WIN32_WINNT	0x0601
#define _WIN32_IE	0x0700
#define _RICHEDIT_VER	0x0300
#define _SILENCE_STDEXT_HASH_DEPRECATION_WARNINGS
#define _CRT_SECURE_NO_WARNINGS

#include <atlbase.h>
#include <assert.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlframe.h>
#include <atlctrls.h>
#include <atldlgs.h>
#include <atlctrlw.h>
#include <atlsplit.h>
#include <atlmisc.h>
#include <atlstr.h>

using namespace WTL;

#define VK_PAGE_UP 0x021
#define VK_PAGE_DOWN 0x22
#define VK_PLUS 0x6b
#define VK_MINUS 0x6d

// Common definitions (Project wide)
#include <vector>
#include <list>
#include <map>
#include <string>

// Force referencing common helpers
#include "..\JPEGView\Helpers.h"
