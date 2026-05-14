#ifndef _RAR_ERRHANDLER_
#define _RAR_ERRHANDLER_

#include "os.hpp"
#include "rartypes.hpp"

enum RAR_EXIT {
  RARX_SUCCESS=0, RARX_WARNING=1, RARX_FATAL=2, RARX_CRC=3,
  RARX_LOCK=4, RARX_WRITE=5, RARX_OPEN=6, RARX_USERERROR=7,
  RARX_MEMORY=8, RARX_CREATE=9, RARX_NOFILES=10, RARX_BADPWD=11,
  RARX_READ=12, RARX_BADARC=13, RARX_USERBREAK=255
};

class ErrorHandler
{
  private:
    RAR_EXIT ExitCode = RARX_SUCCESS;
    uint ErrCount = 0;
    // bool EnableBreak = false;
    bool Silent = true;
    bool DisableShutdown = false;
    // bool ReadErrIgnoreAll = false;
  public:
    ErrorHandler() {}
    void Clean() { ExitCode = RARX_SUCCESS; ErrCount = 0; }

    // All error reporting is silenced for QuickView embedded use.
    void MemoryError() { ExitCode = RARX_MEMORY; ErrCount++; }
    void OpenError(const std::wstring &/*FileName*/) { ExitCode = RARX_OPEN; ErrCount++; }
    void CloseError(const std::wstring &/*FileName*/) {}
    void ReadError(const std::wstring &/*FileName*/) { ExitCode = RARX_READ; ErrCount++; }
    void AskRepeatRead(const std::wstring &/*FileName*/, bool &/*Ignore*/, bool &/*Retry*/, bool &Quit) { Quit = true; }
    void WriteError(const std::wstring &/*ArcName*/, const std::wstring &/*FileName*/) {}
    void WriteErrorFAT(const std::wstring &/*FileName*/) {}
    bool AskRepeatWrite(const std::wstring &/*FileName*/, bool /*DiskFull*/) { return false; }
    void SeekError(const std::wstring &/*FileName*/) {}
    void GeneralErrMsg(const wchar */*fmt*/, ...) {}
    void MemoryErrorMsg() { MemoryError(); }
    void OpenErrorMsg(const std::wstring &FileName) { OpenError(FileName); }
    void OpenErrorMsg(const std::wstring &/*ArcName*/, const std::wstring &/*FileName*/) {}
    void CreateErrorMsg(const std::wstring &/*FileName*/) {}
    void CreateErrorMsg(const std::wstring &/*ArcName*/, const std::wstring &/*FileName*/) {}
    void ReadErrorMsg(const std::wstring &FileName) { ReadError(FileName); }
    void ReadErrorMsg(const std::wstring &/*ArcName*/, const std::wstring &/*FileName*/) {}
    void WriteErrorMsg(const std::wstring &/*ArcName*/, const std::wstring &/*FileName*/) {}
    void ArcBrokenMsg(const std::wstring &/*ArcName*/) {}
    void ChecksumFailedMsg(const std::wstring &/*ArcName*/, const std::wstring &/*FileName*/) {}
    void UnknownMethodMsg(const std::wstring &/*ArcName*/, const std::wstring &/*FileName*/) {}
    void Exit(RAR_EXIT Code) { ExitCode = Code; }
    void SetErrorCode(RAR_EXIT Code) { ExitCode = Code; }
    RAR_EXIT GetErrorCode() { return ExitCode; }
    uint GetErrorCount() { return ErrCount; }
    void SetSignalHandlers(bool /*Enable*/) {}
    void Throw(RAR_EXIT Code) { Exit(Code); } // No C++ exceptions.
    void SetSilent(bool Mode) { Silent = Mode; }
    bool GetSysErrMsg(std::wstring &/*Msg*/) { return false; }
    void SysErrMsg() {}
    int GetSystemErrorCode() { return 0; }
    void SetSystemErrorCode(int /*Code*/) {}
    void SetDisableShutdown() { DisableShutdown = true; }
    bool IsShutdownEnabled() { return !DisableShutdown; }

    bool UserBreak = false;
    bool MainExit = false;
};

#endif
