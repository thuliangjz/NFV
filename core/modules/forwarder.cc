#include "forwarder.h"

#include"../utils/ether.h"
#include"../utils/ip.h"
#include"../utils/udp.h"
#include"../utils/copy.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;
using bess::utils::Copy;
using bess::utils::CopyInlined;
using std::min;

const Commands Forwarder::cmds = {
    {"set_proto_rw_pos", "SetProtoRWPos",
     MODULE_CMD_FUNC(&Forwarder::SetProtoRWPos), Command::THREAD_UNSAFE},
    {"set_postcard", "SetPostcard", 
     MODULE_CMD_FUNC(&Forwarder::SetPostcard), Command::THREAD_UNSAFE},
     {"add_gate_group", "AddGateGroup", 
     MODULE_CMD_FUNC(&Forwarder::AddGateGroup), Command::THREAD_UNSAFE},
     {"clear_gate_group", "ClearGateGroup", 
     MODULE_CMD_FUNC(&Forwarder::ClearGateGroup), Command::THREAD_UNSAFE}
};

CommandResponse Forwarder::SetProtoRWPos(const bess::nft::FwdSetProtoRWPosArg &arg){
    auto it = _gate_groups.find(arg.igate_idx());
    if(it == _gate_groups.end()){
        return CommandFailure(EINVAL, "gate doesnot exist");
    }
    it->second.read_pos = arg.read_pos();
    it->second.write_pos = arg.write_pos();
    return CommandSuccess();
}

CommandResponse Forwarder::SetPostcard(const bess::nft::FwdSetPostcardArg& arg) {
    auto it = _gate_groups.find(arg.igate_idx());
    if(it == _gate_groups.end()){
        return CommandFailure(EINVAL, "gate doesnot exist");
    }
    it->second.delimi_pos = arg.delimi_pos();
    it->second.proto_type = arg.protocol_number();
    return CommandSuccess();
}

CommandResponse Forwarder::AddGateGroup(const bess::nft::FwdAddGateGroupArg &arg){
    if(arg.type() != IGATE_T && arg.type() != EGATE_T){
        return CommandFailure(EINVAL, "invalid gate type");
    }
    auto it = _gate_groups.find(arg.igate_idx());
    if(it != _gate_groups.end()){
        return CommandFailure(EINVAL, "gate already exists");
    }
    _gate_groups[arg.igate_idx()] = {
        .report_id = static_cast<uint8_t>(arg.report_id()),
        .type = static_cast<uint8_t>(arg.type()),
        .read_pos = sizeof(Ipv4) + sizeof(Ethernet),
        .write_pos = sizeof(Ipv4) + sizeof(Ethernet),
        .delimi_pos = sizeof(Ipv4) + sizeof(Ethernet) + sizeof(Udp),
        .proto_type = PROTO_TYPE_ETH,
        .count_pkt = 0,
        .count_bytes = 0,
        .reset_time = 0,
        .reset_interval = static_cast<int>(arg.rate_interval()),
    };
    return CommandSuccess();
}

CommandResponse Forwarder::ClearGateGroup(const bess::nft::NFTEmptyArg&){
    _gate_groups.clear();
    return CommandSuccess();
}

CommandResponse Forwarder::Init(const bess::nft::FwdArg& arg) {
    _id = arg.id();
    return CommandSuccess();
}

inline INFTHeader* Forwarder::GetINFTHeader(gate_idx_t gate, bess::Packet *pkt){
    return reinterpret_cast<INFTHeader*>(pkt->head_data<char*>() + _gate_groups[gate].read_pos);
}

inline void Forwarder::TagTimeStamp(gate_idx_t gate, bess::Packet *pkt, uint64_t arrive_time){
    INFTHeader *inft = GetINFTHeader(gate, pkt);
    INFTData *inft_data = (reinterpret_cast<INFTData*>(inft + 1)) + (inft->length << 2);
    inft_data->latency = arrive_time;
}

inline void Forwarder::WriteTimeStamp(gate_idx_t gate, bess::Packet *pkt, uint64_t leave_time){
    INFTHeader *inft = GetINFTHeader(gate, pkt);
    INFTData *inft_data = (reinterpret_cast<INFTData*>(inft + 1)) + (inft->length << 2);
    inft_data->latency = leave_time - inft_data->latency;
}

void Forwarder::WriteRate(gate_idx_t gate, bess::Packet *pkt, uint16_t metric){
    INFTHeader *inft = GetINFTHeader(gate, pkt);
    INFTData *inft_data = (reinterpret_cast<INFTData*>(inft + 1)) + (inft->length << 2);
    const auto &gate_info = _gate_groups[gate];
    if(gate_info.type == EGATE_T &&
        (metric == METRIC_RATE_E_B || metric == METRIC_RATE_E_P)){
        inft_data->rate = metric == METRIC_RATE_E_B ? gate_info.count_bytes : gate_info.count_pkt;
    }
    if(gate_info.type == IGATE_T && 
        (metric == METRIC_RATE_I_B || metric == METRIC_RATE_I_P)) {
        inft_data->rate = metric == METRIC_RATE_I_B ? gate_info.count_bytes : gate_info.count_pkt;
    }
}

void Forwarder::GeneratePostcard(gate_idx_t gate, bess::Packet *pkt, uint64_t leave_time){
    int p_l = _gate_groups[gate].delimi_pos;
    int s = _que_pkts.empty() ? 0 : _que_pkts.front().first;
    int e = _que_pkts.empty() ? 0 : _que_pkts.back().second;
    //寻找空间
    while((s < e && FORWARDER_BUFFER_SIZE - e < p_l && s < p_l) || 
        (s > e && e - s < p_l)){
        _que_pkts.pop();
        if(_que_pkts.empty())
            return;
        s = _que_pkts.front().first;
    }
   //确定起始位置
    int cp_start;
    if(s < e){
        if(p_l < FORWARDER_BUFFER_SIZE - e)
            cp_start = e;
        else
            cp_start = 0;
    }
    else
        cp_start = e;
    //填写postcard头部
    PostcardHeader* p = reinterpret_cast<PostcardHeader*> (_postcard_buffer + cp_start);
    p->version = 0;
    p->nxp = _gate_groups[gate].proto_type;
    p->id = _id;
    //根据之前记录的igress信息填写
    INFTHeader *inft = GetINFTHeader(gate, pkt);
    INFTData *inft_data = (reinterpret_cast<INFTData*>(inft + 1)) + (inft->length << 2);
    p->igress = inft_data->igress;
    p->egress = inft_data->egress;
    p->seq = _seq_postcard_nxt++;
    p->leave_time = leave_time;
    //复制其他部分
    CopyPacketHeader(gate, _postcard_buffer + cp_start + sizeof(PostcardHeader), pkt);

    _que_pkts.push(std::make_pair(cp_start, 
        cp_start + _gate_groups[gate].delimi_pos + sizeof(PostcardHeader)));
    _buffer_start = _que_pkts.front().first;
    _buffer_end = _que_pkts.back().second;
}

void Forwarder::CopyPacketHeader(gate_idx_t gate, char* dst, bess::Packet *pkt){
    //已经预留了postcard头部的位置
    int r = _gate_groups[gate].read_pos, d = _gate_groups[gate].delimi_pos;
    int p_l = pkt->data_len();
    if(r < d){
        //包头中包含了INFT协议，需要去掉
        Copy(dst, pkt->head_data<char*>(), r);
        Copy(dst + r, pkt->head_data<char*>(r + LENGTH_INFT), 
            min(d + LENGTH_INFT, p_l) - (r + LENGTH_INFT));
    }
    else {
        Copy(dst, pkt->head_data<char*>(), d);
    }
}

void Forwarder::RewriteCheck(gate_idx_t gate, bess::Packet *pkt){
    const auto &gate_info = _gate_groups[gate];
    if(gate_info.read_pos == gate_info.write_pos)
        return;
    Copy(_rewrite_buffer, pkt->head_data<char*>(gate_info.read_pos), LENGTH_INFT);
    //因为存在移动，所以使用memmove
    memmove(pkt->head_data<char*>(gate_info.read_pos),
        pkt->head_data<char*>(gate_info.read_pos + LENGTH_INFT),
        pkt->data_len() - gate_info.read_pos - LENGTH_INFT);
    memmove(pkt->head_data<char*>(gate_info.write_pos + LENGTH_INFT), 
        pkt->head_data<char*>(gate_info.write_pos),
        pkt->data_len() - gate_info.write_pos - LENGTH_INFT);
    Copy(pkt->head_data<char*>(gate_info.write_pos), _rewrite_buffer, LENGTH_INFT);
}

void Forwarder::ProcessBatch(Context* ctx, bess::PacketBatch *batch){
    //从入口处获取信息
    gate_idx_t igate = ctx->current_igate;
    auto it = _gate_groups.find(igate);
    uint32_t current_us = tsc_to_us(rdtsc());
    if(it == _gate_groups.end()){
        //从未登记的门进入不做任何处理
        bess::Packet::Free(batch);
        return;
    }
    int bytes_read = 0;
    for(int i = 0; i < batch->cnt(); ++i){
        bess::Packet *pkt = batch->pkts()[i];
        INFTHeader *inft = reinterpret_cast<INFTHeader*>(pkt->head_data<char*>(it->second.read_pos));
        if(inft->preamble == MAGIC_NFT){
            //标记id，igress和egress
            INFTData *inft_data = reinterpret_cast<INFTData*>(
                (reinterpret_cast<char*>(inft + 1)) + (inft->length << 4));
            inft_data->id = _id;
            inft_data->igress = it->second.type == IGATE_T ? it->second.report_id : inft_data->igress;
            inft_data->egress = it->second.type == EGATE_T ? it->second.report_id : inft_data->egress;

            if(inft->is_postcard && it->second.type == EGATE_T){
                //每一批包的postcard用同一个时间戳
                GeneratePostcard(igate, pkt, current_us);
            }
            else{
                uint16_t metric = inft->type_metric;
                switch(metric){
                    case METRIC_LATENCY:
                        if(it->second.type == IGATE_T)
                            TagTimeStamp(igate, pkt, current_us);
                        else
                            WriteTimeStamp(igate, pkt, current_us);
                        break;
                    case METRIC_RATE_I_B:
                    case METRIC_RATE_I_P:
                    case METRIC_RATE_E_B:
                    case METRIC_RATE_E_P:
                        WriteRate(igate, pkt, metric);     //对于吞吐量类型的数据，在入口和出口处都写，相当于一个forwarder会插入两条数据
                        break;
                    default:
                        break;
                }
                //更新INFT头中length域的长度
                inft->length += 1;
            }
        }
        bytes_read += pkt->data_len();
        RewriteCheck(igate, pkt);
        EmitPacket(ctx, pkt, igate);    //发送到对应的镜像出口
    }
    it->second.count_bytes += bytes_read;
    it->second.count_pkt += batch->cnt();
    double now = tsc_to_us(rdtsc());
    if(now - it->second.reset_time > it->second.reset_interval){
        //重置
        it->second.count_bytes = 0;
        it->second.count_pkt = 0;
        it->second.reset_time = now;
    }
}

struct task_result Forwarder::RunTask(Context* ctx, bess::PacketBatch *batch, void*){
    if(children_overload_ > 0){
        return {
                .block = true, .packets = 0, .bits = 0,
        };
    }
    int cnt = bess::Packet::Alloc(batch->pkts(), bess::PacketBatch::kMaxBurst, 100);
    batch->set_cnt(cnt);
    int i = 0;
    int size_total = 0;
    while(i < cnt && !_que_pkts.empty()){
        //将_que中的包拷贝到batch中
        int size = _que_pkts.front().second - _que_pkts.front().first;
        batch->pkts()[i]->set_data_off(SNBUF_HEADROOM);
        batch->pkts()[i]->set_total_len(size);
        batch->pkts()[i]->set_data_len(size);
        CopyInlined(batch->pkts()[i], _postcard_buffer + size, size);
        ++i;
        _que_pkts.pop();
        size_total += size;
        EmitPacket(ctx, batch->pkts()[i], FORWARDER_POSTCARD_GATE);
    }
    return {.block = (cnt == 0),
            .packets = static_cast<uint32_t>(cnt),
            .bits = static_cast<uint64_t>(size_total) * 8};
}
