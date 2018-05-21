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
    {"set_proto_rwm", "FwdSetProtoRWMArg",
     MODULE_CMD_FUNC(&Forwarder::SetProtoRWM), Command::THREAD_UNSAFE},
    {"set_postcard", "FwdSetPostcardArg", 
     MODULE_CMD_FUNC(&Forwarder::SetPostcard), Command::THREAD_UNSAFE},
     {"add_gate_group", "FwdAddGateGroupArg", 
     MODULE_CMD_FUNC(&Forwarder::AddGateGroup), Command::THREAD_UNSAFE},
     {"clear_gate_group", "NFTEmptyArg", 
     MODULE_CMD_FUNC(&Forwarder::ClearGateGroup), Command::THREAD_UNSAFE}
};

CommandResponse Forwarder::SetProtoRWM(const bess::nft::FwdSetProtoRWMArg &arg){
    auto it = _gate_groups.find(arg.igate_idx());
    if(it == _gate_groups.end()){
        return CommandFailure(EINVAL, "gate doesnot exist");
    }
    if(arg.modify_content().length() > FORWARDER_MAX_MODIFY_LENGTH){
        return CommandFailure(EINVAL, "modify content too long");
    }
    it->second.read_pos = arg.read_pos();
    it->second.write_pos = arg.write_pos();
    it->second.modify = arg.modify();
    it->second.modify_pos = static_cast<int>(arg.modify_pos());
    it->second.modify_content = arg.modify_content();
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
    std::string s;
    _gate_groups[arg.igate_idx()] = {
        .report_id = static_cast<uint8_t>(arg.report_id()),
        .type = static_cast<uint8_t>(arg.type()),
        .read_pos = sizeof(Ipv4) + sizeof(Ethernet),
        .write_pos = sizeof(Ipv4) + sizeof(Ethernet),
        .delimi_pos = sizeof(Ipv4) + sizeof(Ethernet) + sizeof(Udp),
        .proto_type = PROTO_TYPE_ETH,
        .modify = false,
        .modify_pos = 0,
        .modify_content = s,
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

CommandResponse Forwarder::Init(const bess::nft::ForwarderArg& arg) {
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
    if(gate_info.read_pos != gate_info.write_pos){
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
    //重写
    if(gate_info.modify){
        Copy(pkt->head_data<char*>(gate_info.modify_pos), gate_info.modify_content.c_str(), 
        gate_info.modify_content.length());
    }
}

void Forwarder::ProcessBatch(Context* ctx, bess::PacketBatch *batch){
    //从入口处获取信息
    /*
    for(int i = 0; i < batch->cnt(); ++i){
        bess::Packet *pkt = batch->pkts()[i];
        INFTHeader *inft = reinterpret_cast<INFTHeader*>(pkt->head_data<char*>(read_pos));
        if(inft->preamble == MAGIC_NFT){
            //标记id，igress和egress
            INFTData *inft_data = reinterpret_cast<INFTData*>(
                (reinterpret_cast<char*>(inft + 1)) + (inft->length << 4));
            inft_data->id = _id;
            inft_data->igress = type == IGATE_T ? report_id : inft_data->igress;
            inft_data->egress = type == EGATE_T ? report_id : inft_data->egress;

            if(inft->is_postcard && type == EGATE_T){
                //每一批包的postcard用同一个时间戳
                GeneratePostcard(igate, pkt, current_us);
            }
            else{
                uint16_t metric = inft->type_metric;
                switch(metric){
                    case METRIC_LATENCY:
                        if(type == IGATE_T)
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
            RewriteCheck(igate, pkt);
        }
        bytes_read += pkt->data_len();
        //EmitPacket(ctx, pkt, igate);    //发送到对应的镜像出口
    }
    */
    /*
    //单遍版本
    gate_idx_t igate = ctx->current_igate;
    auto it = _gate_groups.find(igate);
    const uint32_t current_us = tsc_to_us(rdtsc());
    if(it == _gate_groups.end()){
        //从未登记的门进入不做任何处理
        bess::Packet::Free(batch);
        return;
    }
    int bytes_read = 0;
    const uint32_t read_pos = it->second.read_pos;
    const uint32_t write_pos = it->second.write_pos;
    register uint8_t report_id = it->second.report_id;
    const uint8_t type = it->second.type;
    const uint16_t count_pkt = it->second.count_pkt;
    const uint16_t count_bytes = it->second.count_bytes;
    const bool modify = it->second.modify;
    //赋值表
    uint16_t results[16] = {0};
    //赋值表中与数据包内容无关的项
    results[0] = current_us;
    results[1] = count_pkt;
    results[2] = count_bytes;
    results[11] = count_pkt;
    results[12] = count_bytes;

    for(int i = 0; i < batch->cnt(); ++i){
        bess::Packet *pkt = batch->pkts()[i];
        INFTHeader *inft = reinterpret_cast<INFTHeader*>(pkt->head_data<char*>(read_pos));
        //if(inft->preamble == MAGIC_NFT){
        //标记id，igress和egress
        INFTData *inft_data = reinterpret_cast<INFTData*>(
            (reinterpret_cast<char*>(inft + 1)) + (inft->length << 4));
        inft_data->id = _id;
        inft_data->igress = type == IGATE_T ? report_id : inft_data->igress;
        inft_data->egress = type == EGATE_T ? report_id : inft_data->egress;
        if(!inft->is_postcard){
            //为每一个包计算赋值数据表
            results[3] = inft_data->rate;
            results[4] = inft_data->rate;
            results[8] = current_us - inft_data->latency;
            results[9] = inft_data->rate;
            results[10] = inft_data->rate;
            uint16_t metric = inft->type_metric;
            inft_data->latency = results[(type<<3)|metric];
            //在仅出口处更新数据包长度!
            inft->length += type == EGATE_T ? 1 : 0;
        }
        else if(inft->is_postcard && type == EGATE_T){
            //每一批包的postcard用同一个时间戳
            GeneratePostcard(igate, pkt, current_us);
        }
        if(read_pos != write_pos || modify)
            RewriteCheck(igate, pkt);
        //}
        bytes_read += pkt->data_len();
    }
    */
    /*
    //两路展开纯INFT版本
    gate_idx_t igate = ctx->current_igate;
    auto it = _gate_groups.find(igate);
    const uint32_t current_us = tsc_to_us(rdtsc());
    if(it == _gate_groups.end()){
        //从未登记的门进入不做任何处理
        bess::Packet::Free(batch);
        return;
    }
    int bytes_read = 0;
    const uint32_t read_pos = it->second.read_pos;
    register uint8_t report_id = it->second.report_id;
    const uint8_t type = it->second.type;
    const uint16_t count_pkt = it->second.count_pkt;
    const uint16_t count_bytes = it->second.count_bytes;
    //赋值表
    uint16_t results[16] = {0};
    //赋值表中与数据包内容无关的项
    results[0] = current_us;
    results[1] = count_pkt;
    results[2] = count_bytes;
    results[11] = count_pkt;
    results[12] = count_bytes;

    uint16_t results1[16] = {0};
    //赋值表中与数据包内容无关的项
    results1[0] = current_us;
    results1[1] = count_pkt;
    results1[2] = count_bytes;
    results1[11] = count_pkt;
    results1[12] = count_bytes;


    for(int i = 0; i < batch->cnt(); i += 2){
        bess::Packet *pkt = batch->pkts()[i];
        bess::Packet *pkt1 = batch->pkts()[i + 1];

        INFTHeader *inft = reinterpret_cast<INFTHeader*>(pkt->head_data<char*>(read_pos));
        INFTHeader *inft1 = reinterpret_cast<INFTHeader*>(pkt1->head_data<char*>(read_pos));
        //if(inft->preamble == MAGIC_NFT){
        //标记id，igress和egress
        INFTData *inft_data = reinterpret_cast<INFTData*>((reinterpret_cast<char*>(inft + 1)) + (inft->length << 4));
        INFTData *inft_data1 = reinterpret_cast<INFTData*>((reinterpret_cast<char*>(inft1 + 1)) + (inft1->length << 4));

        inft_data->id = _id;
        inft_data1->id = _id;

        inft_data->igress = type == IGATE_T ? report_id : inft_data->igress;
        inft_data->egress = type == IGATE_T ? report_id : inft_data1->igress;
        

        inft_data->egress = type == EGATE_T ? report_id : inft_data->egress;
        inft_data1->egress = type == EGATE_T ? report_id : inft_data1->egress;

        //为每一个包计算赋值数据表
        results[3] = inft_data->rate;
        results1[3] = inft_data1->rate;

        results[4] = inft_data->rate;
        results1[4] = inft_data1->rate;

        results[8] = current_us - inft_data->latency;
        results1[8] = current_us - inft_data1->latency;
        
        results[9] = inft_data->rate;
        results1[9] = inft_data1->rate;
        
        results[10] = inft_data->rate;
        results1[10] = inft_data1->rate;
        
        uint16_t metric = inft->type_metric;
        uint16_t metric1 = inft1->type_metric;
        
        inft_data->latency = results[(type<<3)|metric];
        inft_data1->latency = results1[(type<<3)|metric1];
        //在仅出口处更新数据包长度!
    
        inft->length += type == EGATE_T ? 1 : 0;
        inft1->length += type == EGATE_T ? 1 : 0;
       

       bytes_read += pkt->data_len();
       bytes_read += pkt1->data_len();
    }
    */
    /*
    //复制移位(3次复制)
    gate_idx_t igate = ctx->current_igate;
    const uint16_t src = 70, sz = 8;
    const uint16_t read_pos = 34, write_pos = 42;
    const uint16_t cnt = batch->cnt();
    char proto_buffer[40];
    for(int i = 0; i < cnt; ++i){
        bess::Packet *pkt = batch->pkts()[i];
        bess::utils::CopyInlined(proto_buffer,pkt->head_data<char*>(read_pos), LENGTH_INFT);
        bess::utils::CopySmall(pkt->head_data<char*>(read_pos), pkt->head_data<char*>(src), sz);
        bess::utils::CopySmall(pkt->head_data<char*>(write_pos), proto_buffer, LENGTH_INFT);
    }*/

    gate_idx_t igate = ctx->current_igate;
    const uint32_t current_us = tsc_to_us(rdtsc());
    const uint16_t sz_head = 78;
    const int cnt = batch->cnt();
    for(int i = 0; i < cnt; ++i) {
        bess::Packet *pkt = batch->pkts()[i];
        bess::Packet *pkt_postcard = bess::Packet::Alloc();
        if(!pkt_postcard){
            break;
        }
        pkt_postcard->set_data_off(SNBUF_HEADROOM);
        pkt_postcard->set_total_len(sizeof(PostcardHeader) + sz_head);
        pkt_postcard->set_data_len(sizeof(PostcardHeader) + sz_head);
        PostcardHeader* ph = reinterpret_cast<PostcardHeader*>(pkt_postcard->head_data<char*>());
        ph->igress = igate;
        ph->seq = _seq_postcard_nxt++;
        ph->leave_time = current_us;
        bess::utils::CopyInlined(ph + 1, pkt->head_data(), sz_head);
        EmitPacket(ctx, pkt, FORWARDER_POSTCARD_GATE);
    }
    /*
    //统计信息更新 
    it->second.count_bytes += bytes_read;
    it->second.count_pkt += batch->cnt();
    if(current_us - it->second.reset_time > it->second.reset_interval){
        //重置
        it->second.count_bytes = 0;
        it->second.count_pkt = 0;
        it->second.reset_time = ctx->current_ns;
    }
    */
    RunChooseModule(ctx, igate, batch);
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

ADD_MODULE(Forwarder, "forwarder", "forwarder for NFT architecture") 
