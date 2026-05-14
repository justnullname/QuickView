#ifndef _RAR_ARCHIVE_
#define _RAR_ARCHIVE_

class PPack;
class RawRead;
class RawWrite;

class CmdExtract {
public:
    static void UnstoreFile(ComprDataIO &DataIO, int64 UnpSize) {
        byte Buffer[0x10000];
        while (UnpSize > 0) {
            int ReadSize = DataIO.UnpRead(Buffer, (size_t)UnpSize < sizeof(Buffer) ? (size_t)UnpSize : sizeof(Buffer));
            if (ReadSize <= 0) break;
            DataIO.UnpWrite(Buffer, ReadSize);
            UnpSize -= ReadSize;
        }
    }
};

enum NOMODIFY_FLAGS 
{
  NMDF_ALLOWLOCK=1,NMDF_ALLOWANYVOLUME=2,NMDF_ALLOWFIRSTVOLUME=4
};

enum RARFORMAT {RARFMT_NONE,RARFMT14,RARFMT15,RARFMT50,RARFMT_FUTURE};

enum ADDSUBDATA_FLAGS
{
  ASDF_SPLIT          = 1,
  ASDF_COMPRESS       = 2,
  ASDF_CRYPT          = 4,
  ASDF_CRYPTIFHEADERS = 8
};

#define MAX_HEADER_SIZE_RAR5 0x200000

class Archive:public File
{
  private:
    void UpdateLatestTime(FileHeader *CurBlock);
    void ConvertNameCase(std::wstring &Name);
    void ConvertFileHeader(FileHeader *hd);
    size_t ReadHeader14();
    size_t ReadHeader15();
    size_t ReadHeader50();
    void ProcessExtra50(RawRead *Raw,size_t ExtraSize,const BaseBlock *bb);
    void UnexpEndArcMsg();
    void BrokenHeaderMsg();
    void UnkEncVerMsg(const std::wstring &Name,const std::wstring &Info);
    bool DoGetComment(std::wstring &CmtData);
    bool ReadCommentData(std::wstring &CmtData);

    ComprDataIO SubDataIO;

    int RecoveryPercent;

    RarTime LatestTime;
    int LastReadBlock;
    HEADER_TYPE CurHeaderType;

    bool SilentOpen;
  public:
    Archive();
    ~Archive();
    static RARFORMAT IsSignature(const byte *D,size_t Size);
    bool IsArchive(bool EnableBroken);
    size_t SearchBlock(HEADER_TYPE HeaderType);
    size_t SearchSubBlock(const wchar *Type);
    size_t SearchRR();
    int GetRecoveryPercent() {return RecoveryPercent;}
    size_t ReadHeader();
    void CheckArc(bool EnableBroken);
    void CheckOpen(const std::wstring &Name);
    bool WCheckOpen(const std::wstring &Name);
    bool GetComment(std::wstring &CmtData);
    void ViewComment();
    void SetLatestTime(RarTime *NewTime);
    void SeekToNext();
    bool CheckAccess();
    bool IsArcDir();
    void ConvertAttributes();
    void VolSubtractHeaderSize(size_t SubSize);
    uint FullHeaderSize(size_t Size);
    int64 GetStartPos();
    void AddSubData(const byte *SrcData,uint64 DataSize,File *SrcFile,
         const wchar *Name,uint Flags);
    bool ReadSubData(std::vector<byte> *UnpData,File *DestFile,bool TestMode);
    HEADER_TYPE GetHeaderType() {return CurHeaderType;}
    void SetSilentOpen(bool Mode) {SilentOpen=Mode;}
    static uint64 GetWinSize(uint64 Size,uint &Flags);

    BaseBlock ShortBlock;
    MarkHeader MarkHead;
    MainHeader MainHead;
    CryptHeader CryptHead;
    FileHeader FileHead;
    EndArcHeader EndArcHead;
    SubBlockHeader SubBlockHead;
    FileHeader SubHead;
    CommentHeader CommHead;
    ProtectHeader ProtectHead;
    EAHeader EAHead;
    StreamHeader StreamHead;

    int64 CurBlockPos;
    int64 NextBlockPos;

    RARFORMAT Format;
    bool Solid;
    bool Volume;
    bool MainComment;
    bool Locked;
    bool Signed;
    bool FirstVolume;
    bool NewNumbering;
    bool Protected;
    bool Encrypted;
    size_t SFXSize;
    bool BrokenHeader;
    bool FailedHeaderDecryption;

    bool Splitting;

    uint VolNumber;
    int64 VolWrite;

    uint64 AddingFilesSize;
    uint64 AddingHeadersSize;

    bool NewArchive;

    std::wstring FirstVolumeName;
};

#endif
