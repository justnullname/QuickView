#ifndef _RAR_OS_
#define _RAR_OS_

#define FALSE 0
#define TRUE  1

// Suppress features we don't use.
#ifndef SILENT
#define SILENT
#endif
#ifndef RAR_NOCRYPT
#define RAR_NOCRYPT
#endif

#include <new>
#include <string>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>

#ifdef _WIN_ALL

#define LITTLE_ENDIAN

#ifndef STRICT
#define STRICT 1
#endif

#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif

#ifndef WINVER
#define WINVER _WIN32_WINNT_WINXP
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT _WIN32_WINNT_WINXP
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <direct.h>
#include <intrin.h>

// Use SSE only for x86/x64, not ARM Windows.
#if defined(_M_IX86) || defined(_M_X64)
  #define USE_SSE
  #define SSE_ALIGNMENT 16
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <io.h>
#include <time.h>
#include <signal.h>
#include <shlobj.h>

#define SIZE_SALT30 8
#define SIZE_SALT50 16
#define SIZE_INITV 16
#define SIZE_PSWCHECK 8
#define SIZE_PSWCHECK_CSUM 4
#define CRYPT_VERSION 0
#define CRYPT5_KDF_LG2_COUNT_MAX 24
#define CRYPT_BLOCK_SIZE 16

enum CRYPT_METHOD { CRYPT_NONE, CRYPT_RAR13, CRYPT_RAR15, CRYPT_RAR20, CRYPT_RAR30, CRYPT_RAR50, CRYPT_UNKNOWN };

class SecPassword {
public:
    bool IsSet() const { return false; }
    void Clean() {}
};

class CommandData;
extern CommandData *Cmd;

#define cleandata(ptr,size) memset(ptr,0,size)


#define SPATHDIVIDER L"\\"
#define CPATHDIVIDER L'\\'
#define MASKALL      L"*"

#define READBINARY   "rb"
#define READTEXT     "rt"
#define UPDATEBINARY "r+b"
#define CREATEBINARY "w+b"
#define WRITEBINARY  "wb"
#define APPENDTEXT   "at"

#define _stdfunction __cdecl
#define _forceinline __forceinline

#define DefConfigName  L"rar.ini"
#define DefLogName     L"rar.log"

#define SAVE_LINKS
#define ENABLE_ACCESS

#endif // _WIN_ALL

typedef const wchar_t* MSGID;

#ifndef SSE_ALIGNMENT
  #define SSE_ALIGNMENT 1
#endif

#if !defined(BIG_ENDIAN) && (defined(_WIN_ALL) || defined(__i386__) || defined(__x86_64__) || defined(__aarch64__))
#define ALLOW_MISALIGNED
#endif

#endif // _RAR_OS_
