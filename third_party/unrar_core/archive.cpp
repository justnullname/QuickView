#include "rar.hpp"

static CommandData StaticCmd;
CommandData *Cmd = &StaticCmd;

// Comment reading not needed for QuickView image extraction.
// Original: #include "arccmt.cpp"


Archive::Archive()
{
  Format = RARFMT_NONE;
  Solid = false;
  Volume = false;
  MainComment = false;
  Locked = false;
  Signed = false;
  FirstVolume = false;
  NewNumbering = false;
  SFXSize = 0;
  LatestTime.Reset();
  Protected = false;
  Encrypted = false;
  FailedHeaderDecryption = false;
  BrokenHeader = false;
  LastReadBlock = 0;
  CurHeaderType = HEAD_UNKNOWN;
  CurBlockPos = 0;
  NextBlockPos = 0;
  RecoveryPercent = -1;
  MainHead.Reset();
  CryptHead = {};
  EndArcHead.Reset();
  VolNumber = 0;
  VolWrite = 0;
  AddingFilesSize = 0;
  AddingHeadersSize = 0;
  Splitting = false;
  NewArchive = false;
  SilentOpen = true; // Always silent in QuickView.
}


Archive::~Archive()
{
}


void Archive::CheckArc(bool EnableBroken)
{
  if (!IsArchive(EnableBroken))
    ErrHandler.Exit(RARX_BADARC);
}


#if !defined(SFX_MODULE)
void Archive::CheckOpen(const std::wstring &Name)
{
  TOpen(Name);
  CheckArc(false);
}
#endif


bool Archive::WCheckOpen(const std::wstring &Name)
{
  if (!WOpen(Name))
    return false;
  if (!IsArchive(false))
  {
    Close();
    return false;
  }
  return true;
}


RARFORMAT Archive::IsSignature(const byte *D, size_t Size)
{
  RARFORMAT Type = RARFMT_NONE;
  if (Size >= 1 && D[0] == 0x52)
#ifndef SFX_MODULE
    if (Size >= 4 && D[1] == 0x45 && D[2] == 0x7e && D[3] == 0x5e)
      Type = RARFMT14;
    else
#endif
      if (Size >= 7 && D[1] == 0x61 && D[2] == 0x72 && D[3] == 0x21 && D[4] == 0x1a && D[5] == 0x07)
      {
#ifndef SFX_MODULE
        if (D[6] == 0)
          Type = RARFMT15;
        else
#endif
          if (D[6] == 1)
            Type = RARFMT50;
          else
            if (D[6] > 1 && D[6] < 5)
              Type = RARFMT_FUTURE;
      }
  return Type;
}


bool Archive::IsArchive(bool EnableBroken)
{
  Encrypted = false;
  BrokenHeader = false;

  if (Read(MarkHead.Mark, SIZEOF_MARKHEAD3) != SIZEOF_MARKHEAD3)
    return false;
  SFXSize = 0;

  RARFORMAT Type;
  if ((Type = IsSignature(MarkHead.Mark, SIZEOF_MARKHEAD3)) != RARFMT_NONE)
  {
    Format = Type;
    if (Format == RARFMT14)
      Seek(Tell() - SIZEOF_MARKHEAD3, SEEK_SET);
  }
  else
  {
    std::vector<char> Buffer(MAXSFXSIZE);
    const long CurPos = 1;
    Seek(CurPos, SEEK_SET);
    int ReadSize = Read(Buffer.data(), Buffer.size() - 16);
    for (int I = 0; I < ReadSize; I++)
      if (Buffer[I] == 0x52 && (Type = IsSignature((byte *)&Buffer[I], ReadSize - I)) != RARFMT_NONE)
      {
        Format = Type;
        if (Format == RARFMT14 && I > 0 && CurPos < 28 && ReadSize > 31)
        {
          char *D = &Buffer[28 - CurPos];
          if (D[0] != 0x52 || D[1] != 0x53 || D[2] != 0x46 || D[3] != 0x58)
            continue;
        }
        SFXSize = CurPos + I;
        Seek(SFXSize, SEEK_SET);
        if (Format == RARFMT15 || Format == RARFMT50)
          Read(MarkHead.Mark, SIZEOF_MARKHEAD3);
        break;
      }
    if (SFXSize == 0)
      return false;
  }
  if (Format == RARFMT_FUTURE)
    return false;
  if (Format == RARFMT50)
  {
    if (Read(MarkHead.Mark + SIZEOF_MARKHEAD3, 1) != 1 || MarkHead.Mark[SIZEOF_MARKHEAD3] != 0)
      return false;
    MarkHead.HeadSize = SIZEOF_MARKHEAD5;
  }
  else
    MarkHead.HeadSize = SIZEOF_MARKHEAD3;

  bool HeadersLeft;
  bool StartFound = false;
  while ((HeadersLeft = (ReadHeader() != 0)) == true)
  {
    SeekToNext();
    HEADER_TYPE HType = GetHeaderType();
    StartFound = HType == HEAD_MAIN || (SilentOpen && HType == HEAD_CRYPT);
    if (StartFound)
      break;
  }

  if (FailedHeaderDecryption && !EnableBroken)
    return false;

  if (BrokenHeader || !StartFound)
  {
    if (!EnableBroken)
      return false;
  }

  MainComment = MainHead.CommentInHeader;

  if (HeadersLeft && !Encrypted && IsSeekable())
  {
    int64 SavePos = Tell();
    int64 SaveCurBlockPos = CurBlockPos, SaveNextBlockPos = NextBlockPos;
    HEADER_TYPE SaveCurHeaderType = CurHeaderType;

    while (ReadHeader() != 0)
    {
      HEADER_TYPE HeaderType = GetHeaderType();
      if (HeaderType == HEAD_SERVICE)
        FirstVolume = Volume && !SubHead.SplitBefore;
      else if (HeaderType == HEAD_FILE)
      {
        FirstVolume = Volume && !FileHead.SplitBefore;
        break;
      }
      else if (HeaderType == HEAD_ENDARC)
        break;
      SeekToNext();
    }
    CurBlockPos = SaveCurBlockPos;
    NextBlockPos = SaveNextBlockPos;
    CurHeaderType = SaveCurHeaderType;
    Seek(SavePos, SEEK_SET);
  }
  if (!Volume || FirstVolume)
    FirstVolumeName = FileName;

  return true;
}


void Archive::SeekToNext()
{
  Seek(NextBlockPos, SEEK_SET);
}


uint Archive::FullHeaderSize(size_t Size)
{
  if (Encrypted)
  {
    Size = ALIGN_VALUE(Size, CRYPT_BLOCK_SIZE);
    if (Format == RARFMT50)
      Size += SIZE_INITV;
    else
      Size += SIZE_SALT30;
  }
  return uint(Size);
}




uint64 Archive::GetWinSize(uint64 Size, uint &Flags)
{
  Flags = 0;
  if (Size < 0x20000 || Size > 0x10000000000ULL)
    return 0;
  uint64 Pow2 = 0x20000;
  for (; 2 * Pow2 <= Size; Pow2 *= 2)
    Flags += FCI_DICT_BIT0;
  if (Size == Pow2)
    return Size;
  uint64 Fraction = (Size - Pow2) / (Pow2 / 32);
  Flags += (uint)Fraction * FCI_DICT_FRACT0;
  return Pow2 + Fraction * (Pow2 / 32);
}
