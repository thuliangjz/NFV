#include "classifier.h"

#include "../utils/ether.h"
#include "../utils/ip.h"
#include "../utils/udp.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Udp;

//各个参数的意义:在交互式环境(python)下调用Command时的函数，传入参数在protobuf中定义的名称，处理函数，是否是线程安全的

const Commands Classifier::cmds = {
    {"add_flow_spec", "ClsAddSpecArg", MODULE_CMD_FUNC(&Classifier::AddTeleSpec),
     Command::THREAD_UNSAFE},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&Classifier::Clear), 
     Command::THREAD_UNSAFE}
};

Classifier::Classifier():Module(), _offset_insert(sizeof(Ethernet) + sizeof(Ipv4)){

}

CommandResponse Classifier::Init(const bess::nft::ClsArg&){
    return CommandSuccess();
}

CommandResponse Classifier::AddTeleSpec(const bess::nft::ClsAddSpecArg& arg){
    //为每一个传入的Spec消息构建一个TelemetrySpec
    for(const auto &spec : arg.specs()){
        TelemetrySpec tmpSpec = {
            .src_ip = Ipv4Prefix(spec.src_ip()),
            .dst_ip = Ipv4Prefix(spec.dst_ip()),
            .src_port = be16_t(static_cast<uint16_t>(spec.src_port())),
            .dst_port = be16_t(static_cast<uint16_t>(spec.dst_port())),
            .is_postcard = spec.is_postcard(),
            .tele_types = {0},
            .tele_index = 0,
            .tele_type_cnt = static_cast<char>(spec.tele_type_size()),
        };
        for(int i = 0; i < spec.tele_type_size(); ++i){
            tmpSpec.tele_types[i] = static_cast<char>(spec.tele_type(i));
        }
        _specs.push_back(tmpSpec);
    }
    return CommandSuccess();
}

CommandResponse Classifier::Clear(const bess::nft::NFTEmptyArg&){
    _specs.clear();
    return CommandSuccess();
}
//TODO: 当前假定所有的数据包都是Eth/IP4/UDP格式
void Classifier::ProcessBatch(Context* ctx, bess::PacketBatch *batch){
    //下面使用的结构体都是定义在utils中的同名.h文件下
    int cnt_pkt = batch->cnt();
    for(int i = 0; i < cnt_pkt; ++i){
        //如果数据包match某个传入的spec，则在数据包中添加测量包
        bess::Packet *pkt = batch->pkts()[i];

        Ethernet *eth = pkt->head_data<Ethernet *>();
        Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);
        size_t ip_bytes = ip->header_length << 2;
        Udp *udp = reinterpret_cast<Udp *>(reinterpret_cast<uint8_t *>(ip) + ip_bytes);

        for(const auto &spec : _specs){
            if(spec.Match(ip->src, ip->dst, udp->src_port, udp->dst_port)){
                InsertProtocol(pkt, spec);
                break;
            }
        }
    }
    RunNextModule(ctx, batch);
}

void Classifier::InsertProtocol(bess::Packet* pkt, Classifier::TelemetrySpec& spec){
    uint16_t len_original = pkt->data_len();
    pkt->set_data_len(len_original + LENGTH_INFT);
    pkt->set_total_len(len_original + LENGTH_INFT);
    char *ptr = pkt->buffer<char*>() + pkt->data_off();
    memmove(ptr + _offset_insert + LENGTH_INFT, 
        ptr + _offset_insert, LENGTH_INFT);
    //在数据包的IP_Options处直接添加测量的数据包
    INFTHeader *h = reinterpret_cast<INFTHeader*>(ptr + _offset_insert);
    h->preamble = MAGIC_NFT;
    h->version = 0;
    if(!spec.is_postcard){
    //采用轮转的方法指定测量参数同时对spec的计数器进行更新
        h->type_metric = spec.tele_types[static_cast<int>(spec.tele_index)];
        ++spec.tele_index;
        spec.tele_index = spec.tele_index >= spec.tele_type_cnt ?
            0 : spec.tele_index;
        h->is_postcard = 0;
    }
    else{
        h->is_postcard = 1;
    }
    h->length = 0;
    //修改IP header的长度
    Ethernet *eth = pkt->head_data<Ethernet *>();
    Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);
    ip->header_length = 15;
}

ADD_MODULE(Classifier, "classifier", "classifier for NFT architecture") 
