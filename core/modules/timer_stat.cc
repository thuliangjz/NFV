#include "timer.h"
#include<iostream>
#include "../module.h"
#include "../pb/nft_msg.pb.h"

using std::cout;
using std::endl;

class TimerStat final : public Module {
    public:
        TimerStat(): Module(){
            _print_interval = 0;
            _packet_count = 0;
            _clock_total = 0;
            _last_reset = 0;
        }
        CommandResponse Init(const bess::nft::TimerStatArg& arg);
        void ProcessBatch(Context *ctx, bess::PacketBatch *batch);
    private:
        uint32_t _print_interval;
        uint64_t _packet_count;
        uint64_t _clock_total;
        double _last_reset;
};

CommandResponse TimerStat::Init(const bess::nft::TimerStatArg& arg){
    using AccessMode = bess::metadata::Attribute::AccessMode;
    AddMetadataAttr("timestamp_s", 8, AccessMode::kRead);
    AddMetadataAttr("timestamp_e", 8, AccessMode::kRead);
    AddMetadataAttr("batch_count", 4, AccessMode::kRead);
    _print_interval = arg.interval(); 
    return CommandSuccess();
}

void TimerStat::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
    double current_time = tsc_to_us(rdtsc());
    int cnt = batch->cnt();
    if(cnt){
        bess::Packet *pkt = batch->pkts()[0];
        //uint64_t clk = get_attr<uint64_t>(this, ATTR_R_TIMESTAME_E, pkt);  
        //clk -= get_attr<uint64_t>(this, ATTR_R_TIMESTAME_S, pkt);
        uint64_t clk = get_attr_with_offset<uint64_t>(8, pkt);
        clk -= get_attr_with_offset<uint64_t>(0, pkt);
        _clock_total += clk; 
        _packet_count += get_attr_with_offset<uint64_t>(16, pkt);
        //如果超过一定的时间间隔则更新_last_reset
        if(current_time -  _last_reset > (double)_print_interval){
            printf("+++++++++++++++++++++++++++++++++++++++\n");
            //std::cout<<static_cast<double>(_clock_total) / _packet_count<<std::endl;
            double cpp =  (double)_clock_total / _packet_count;
            cout<<_clock_total<<endl;
            cout<<_packet_count<<endl;
            cout<<cpp<<endl;
            cout<<tsc_to_us(cpp)<<endl;
            printf("+++++++++++++++++++++++++++++++++++++++\n");
            _last_reset = current_time;
            _clock_total = 0;
            _packet_count = 0;
        }
    }
    RunNextModule(ctx, batch);
}
ADD_MODULE(TimerStat, "timer_stat", "print timer statistics") 
