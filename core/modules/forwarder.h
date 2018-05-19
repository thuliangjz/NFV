#ifndef FORWARDER_H
#define FORWARDER_H
#include <map>
#include<queue>
#include "../module.h"
#include "nft.h"
#include "../pb/nft_msg.pb.h"
#include "../utils/ip.h"

#define IGATE_T 0
#define EGATE_T 1

#define FORWARDER_BUFFER_SIZE 1048576

using bess::utils::be16_t;
using bess::utils::be32_t;
using bess::utils::Ipv4Prefix;

class Forwarder final : public Module {
    public:
        Forwarder(): Module(){}
        static const Commands cmds;
        CommandResponse Init(const bess::nft::FwdArg &arg);
        CommandResponse SetProtoRWPos(const bess::nft::FwdSetProtoRWPosArg &arg);
        CommandResponse SetPostcard(const bess::nft::FwdSetPostcardArg& arg);
        CommandResponse AddGateGroup(const bess::nft::FwdAddGateGroupArg& arg);
        CommandResponse ClearGateGroup(const bess::nft::EmptyArg& arg);

        void ProcessBatch(Context* ctx, bess::PacketBatch* batch) override;

        //生成postcard的task
        struct task_result RunTask(Context* ctx, bess::PacketBatch *batch, void *arg);

        //支持多个gate，入口有16个，出口17个，除了和入口对应的镜像出口外，还有一个用于发送postcard的出口(编号15)
        static const gate_idx_t kNumOGates = FORWARDER_MAX_EGATE;
        static const gate_idx_t kNumIGates = FORWARDER_MAX_IGATE;

        struct GateGroup {
            uint8_t report_id;      //实际上report_id只有4位可以使用，即必须是0-15之间的值
            uint8_t type;           //镜像组是进forwarder还是出forwarder
            uint32_t read_pos;      //读取INFT header的位置
            uint32_t write_pos;     //写入INFT header的位置
            uint32_t delimi_pos;    //生成postcard是截断位置(不包含INFT_protocol)
            uint32_t proto_type;    //包头的第一层协议类型

            //统计数据
            int count_pkt;
            int count_bytes;
            double reset_time;
            int reset_interval;
        };
    private:
        void GeneratePostcard(gate_idx_t gate,bess::Packet* pkt, uint64_t leave_time);
        void TagTimeStamp(gate_idx_t gate, bess::Packet* pkt, uint64_t arrive_time);       //在入口处打上时间戳
        void WriteTimeStamp(gate_idx_t gate, bess::Packet *pkt, uint64_t leave_time);    //出口处计算时间戳
        void WriteRate(gate_idx_t gate, bess::Packet *pkt, uint16_t metric);         //标出出口数目
        void RewriteCheck(gate_idx_t gate, bess::Packet *pkt);      //在将包发出去之前检查是否需要移动INFT协议的位置

        void CopyPacketHeader(gate_idx_t gate, char* dst, bess::Packet *pkt);
        INFTHeader* GetINFTHeader(gate_idx_t gate, bess::Packet *pkt);

        std::map<gate_idx_t, struct GateGroup> _gate_groups;
        char _postcard_buffer[FORWARDER_BUFFER_SIZE];
        int _buffer_start, _buffer_end;
        std::queue<std::pair<int, int>> _que_pkts;        //buffer中保留_que_pkts的指针
        uint32_t _seq_postcard_nxt;
        uint8_t _id;
        char _rewrite_buffer[LENGTH_INFT];
};


#endif
