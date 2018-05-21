#include"timer.h"

CommandResponse Timer::Init(const bess::nft::TimerArg& arg){
    using AccessMode = bess::metadata::Attribute::AccessMode;
    _attr_type = arg.type();
    AddMetadataAttr("timestamp_s", 8, AccessMode::kWrite);
    AddMetadataAttr("timestamp_e", 8, AccessMode::kWrite);
    AddMetadataAttr("batch_count", 4, AccessMode::kWrite);
    return CommandSuccess();
}

void Timer::ProcessBatch(Context *ctx, bess::PacketBatch *batch){
    int cnt = batch->cnt();
    uint64_t time = rdtsc();
    for(int i = 0; i < cnt; ++i){
        bess::Packet *pkt = batch->pkts()[i];
        set_attr<uint64_t>(this, _attr_type, pkt, time);
        if(_attr_type == ATTR_R_TIMESTAME_S){
            //在入口处记录所有这个batch的包的数目
            set_attr<uint32_t>(this, 2, pkt, cnt);
        }
    }
    RunNextModule(ctx, batch);
}
ADD_MODULE(Timer, "timer", "tag time stamp") 
