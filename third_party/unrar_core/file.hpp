#ifndef _RAR_FILE_
#define _RAR_FILE_

#include "os.hpp"
#include "rartypes.hpp"
#include "timefn.hpp"

#ifdef _WIN_ALL
  typedef HANDLE FileHandle;
  #define FILE_BAD_HANDLE INVALID_HANDLE_VALUE
#else
  typedef int FileHandle;
  #define FILE_BAD_HANDLE -1
#endif

enum FILE_HANDLETYPE {FILE_HANDLENORMAL,FILE_HANDLESTD};
enum FILE_ERRORTYPE {FILE_SUCCESS,FILE_NOTFOUND,FILE_READERROR};
enum FILE_READ_ERROR_MODE {FREM_ASK,FREM_TRUNCATE,FREM_IGNORE};

enum FILE_MODE_FLAGS {
  FMF_READ=0, FMF_UPDATE=1, FMF_WRITE=2, FMF_OPENSHARED=4,
  FMF_OPENEXCLUSIVE=8, FMF_SHAREREAD=16, FMF_STANDARDNAMES=32
};

class File
{
  private:
    // Memory-backed I/O for QuickView zero-copy decompression.
    const byte *m_data = nullptr;
    size_t m_size = 0;
    int64 m_pos = 0;
    bool m_opened = false;

    FILE_HANDLETYPE HandleType = FILE_HANDLENORMAL;
    FILE_READ_ERROR_MODE ReadErrorMode = FREM_ASK;

  protected:
    bool OpenShared = false; // Set by Archive class.

  public:
    std::wstring FileName;
    FILE_ERRORTYPE ErrorType = FILE_SUCCESS;

    File() {}
    virtual ~File() {}

    // === QuickView DOD API ===
    // Attach a memory-mapped region as the file source. No OS file handle.
    void SetMemoryBuffer(const byte *Data, size_t Size) {
      m_data = Data;
      m_size = Size;
      m_pos = 0;
      m_opened = true;
    }

    virtual bool Open(const std::wstring &Name, uint /*Mode*/=FMF_READ) {
      FileName = Name;
      return m_opened; // Only succeeds if memory was attached.
    }

    void TOpen(const std::wstring &Name) { Open(Name); }
    bool WOpen(const std::wstring &Name) { return Open(Name); }

    virtual bool Close() {
      m_opened = false;
      return true;
    }

    virtual int Read(void *Data, size_t Size) {
      if (!m_opened || m_pos >= (int64)m_size) return 0;
      size_t avail = m_size - (size_t)m_pos;
      size_t n = (Size < avail) ? Size : avail;
      if (n > 0) {
        memcpy(Data, m_data + m_pos, n);
        m_pos += n;
      }
      return (int)n;
    }

    int DirectRead(void *Data, size_t Size) { return Read(Data, Size); }

    virtual void Seek(int64 Offset, int Method) {
      if (Method == SEEK_SET) m_pos = Offset;
      else if (Method == SEEK_CUR) m_pos += Offset;
      else if (Method == SEEK_END) m_pos = (int64)m_size + Offset;
      if (m_pos < 0) m_pos = 0;
      if (m_pos > (int64)m_size) m_pos = (int64)m_size;
    }

    bool RawSeek(int64 Offset, int Method) { Seek(Offset, Method); return true; }
    virtual int64 Tell() { return m_pos; }
    virtual int64 FileLength() { return (int64)m_size; }
    virtual bool IsOpened() { return m_opened; }

    void SetHandleType(FILE_HANDLETYPE Type) { HandleType = Type; }
    FILE_HANDLETYPE GetHandleType() { return HandleType; }
    bool IsSeekable() { return true; }
    void SetReadErrorMode(FILE_READ_ERROR_MODE Mode) { ReadErrorMode = Mode; }
    void SetExceptions(bool /*Allow*/) {}
    void SetPreserveAtime(bool /*Preserve*/) {}
    bool IsTruncatedAfterReadError() { return false; }
    void SetLineInputMode(bool /*Mode*/) {}
    byte GetByte() { byte b=0; Read(&b,1); return b; }

    // Stubs for write-side APIs we'll never use.
    bool Write(const void */*Data*/, size_t /*Size*/) { return false; }
    bool Create(const std::wstring &/*Name*/, uint /*Mode*/=FMF_UPDATE|FMF_SHAREREAD) { return false; }
    void Flush() {}
    void Prealloc(int64 /*Size*/) {}
    bool Truncate() { return false; }
    void SetOpenFileTime(RarTime */*ftm*/,RarTime */*ftc*/=NULL,RarTime */*fta*/=NULL) {}
    void SetCloseFileTime(RarTime */*ftm*/,RarTime */*fta*/=NULL) {}
    void GetOpenFileTime(RarTime */*ftm*/,RarTime */*ftc*/=NULL,RarTime */*fta*/=NULL) {}
    int64 Copy(File &/*Dest*/,int64 /*Length*/=INT64NDF) { return 0; }
    void SetAllowDelete(bool /*Allow*/) {}
    FileHandle GetHandle() { return FILE_BAD_HANDLE; }
    static size_t CopyBufferSize() { return 0x400000; }
};

#endif
