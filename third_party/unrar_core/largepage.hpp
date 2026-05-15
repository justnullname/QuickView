#ifndef _RAR_LARGEPAGE_
#define _RAR_LARGEPAGE_

#include "os.hpp"
#include "rartypes.hpp"

class CommandData; // Forward declaration

class LargePageAlloc
{
  private:
    bool UseLargePages = false;
  public:
    LargePageAlloc() {}
    void AllowLargePages(bool Allow) { UseLargePages = Allow; }
    static bool IsPrivilegeAssigned() { return false; }
    static bool AssignPrivilege() { return false; }
    static bool AssignPrivilegeBySid(const std::wstring &/*Sid*/) { return false; }
    static bool AssignConfirmation() { return false; }
    static bool ProcessSwitch(CommandData */*Cmd*/, const wchar_t */*Switch*/) { return false; }

    template <class T> T* new_l(size_t Size, bool Clear=false)
    {
      return Clear ? new T[Size]{} : new T[Size];
    }

    template <class T> void delete_l(T *Addr)
    {
      delete[] Addr;
    }
};

#endif
