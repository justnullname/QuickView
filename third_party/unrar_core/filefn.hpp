#ifndef _RAR_FILEFN_
#define _RAR_FILEFN_

#include "os.hpp"
#include "rartypes.hpp"
#include "timefn.hpp"

bool FileExist(const std::wstring &Name);
bool WildFileExist(const std::wstring &Name);
bool CreateDir(const std::wstring &Name);
bool IsDir(uint Attr);
bool IsDevice(uint Attr);

class FindData {
public:
    std::wstring Name;
    uint64 Size;
    uint Attr;
    RarTime mtime;
};

class FindFile {
public:
    FindFile() {}
    void SetMask(const std::wstring &/*Mask*/) {}
    bool Next(FindData */*FD*/) { return false; }
};

#endif
