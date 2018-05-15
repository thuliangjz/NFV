#ifndef NFT_H
#define NFT_H

//NFT体系结构通用定义

//测量参数的类型
#define COUNT_TELE_TYPE 3
//测试版本INFT的协议长度固定
#define LENGTH_INFT 36
//INFT协议开头的preamble，含义是0x7f N F T
#define MAGIC_NFT 0x7f4e4654

struct [[gnu::packed]] INFTHeader {
    uint32_t preamble;
    #if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t version : 4;
    uint8_t type_metric : 3;
    uint8_t is_postcard : 1;
    #elif __BYTE_ORDER == __BIG_ENDIAN
    uint8_t is_postcard : 1;
    uint8_t type_metric : 3;
    uint8_t version : 4;
    #endif
    uint8_t lenth;  //以4字节为单位
};

struct [[gnu::packed]] INFTData {
    uint8_t id;
    #if __BYTE_ORDER == __LITTLE_ENDIAN
    uint8_t igress : 4;
    uint8_t egress : 4;
    #elif __BYTE_ORDER == __BIG_ENDIAN
    uint8_t egress : 4;
    uint8_t igress : 4;
    #endif
    union {
        uint16_t latency;
        uint16_t iodiff;
        uint16_t irate;
    };
};

#endif