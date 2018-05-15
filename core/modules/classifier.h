#ifndef CLASSIFIER_H
#define CLASSIFIER_H


#include "../module.h"  //继承module类
#include "nft.h"
#include "../pb/nft_msg.pb.h"    //protoc生成的protobuf消息解析支持

#include "../utils/ip.h"    //支持IPV4前缀识别
#include <vector>

class Classifier final : public Module {
    public:
        struct TelemetrySpec {
            bool Match(be32_t sip, be32_t dip, be16_t sport, be16_t dport) const {
                return src_ip.Match(sip) && dst_ip.Match(dip) &&
                        (src_port == be16_t(0) || src_port == sport) &&
                        (dst_port == be16_t(0) || dst_port == dport);
            }
            Ipv4Prefix src_ip;
            Ipv4Prefix dst_ip;
            be16_t src_port;
            be16_t dst_port;
            bool is_postcard;
            char tele_types[COUNT_TELE_TYPE];   //这个Spec对应的测量指标的编号都有哪些
            char tele_index;    //上一个数据包测量的编号在tele_types中的索引
            char tele_tpye_cnt;
        };

        Classifier();        
        static const Commands cmds; //额外支持的command对象

        CommandResponse Init(const bess::nft::ClsArg &arg);

        void ProcessBatch(Context* ctx, bess::PacketBatch* batch) override;

        //添加规则的命令
        CommandResponse AddTeleSpec(const bess::nft::ClsAddSpecArg& arg);
        CommandResponse Clear(const bess:nft::EmptyArg& arg);    //用于清除所有的Spec
    private:
        bool classify(bess::Packet* pkt);
        void InsertProtocol(bess::Packet* pkt, TelemetrySpec& spec); //根据会更新spec的内容 
        std::vector<TelemetrySpec> _specs;
        char _buffer[2048];  //用来插入测量包头的缓冲区，避免反复申请。
        int _offset_insert; //记录插入位置的临时变量
};
#endif
