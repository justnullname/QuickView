#ifndef _RAR_HASH_
#define _RAR_HASH_

#include "os.hpp"
#include "rartypes.hpp"

enum HASH_TYPE { HASH_NONE, HASH_RAR14, HASH_CRC32, HASH_CRC64, HASH_BLAKE2, HASH_SHA256 };

class HashValue {
public:
    void Init(HASH_TYPE Type);
    bool operator == (const HashValue &cmp) const;
    bool Cmp(HashValue */*Expected*/, const byte */*Key*/ = nullptr) { return true; }
    HASH_TYPE Type;
    union {
        uint32 CRC32;
        byte Digest[32];
    };
};

struct blake2sp_state;

class DataHash {
public:
    DataHash();
    ~DataHash();
    void Init(HASH_TYPE Type, uint MaxThreads = 1);
    void Update(const void *Data, size_t DataSize);
    void Result(HashValue *Result);
    uint GetCRC32();
    bool Cmp(HashValue *CmpValue, byte *Key);
    uint BitReverse32(uint N);
    uint gfMulCRC(uint A, uint B);
    uint gfExpCRC(uint N);

    HASH_TYPE HashType;
    uint32 CurCRC32;
    blake2sp_state *blake2ctx;
};

#endif
