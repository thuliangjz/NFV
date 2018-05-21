#ifndef TIMER_H
#define TIMER_H
#include "../module.h"
#include "../pb/nft_msg.pb.h"

#define ATTR_R_TIMESTAME_S 0
#define ATTR_R_TIMESTAME_E 1

class Timer final : public Module {
    public:
        Timer(): Module(){}
        CommandResponse Init(const bess::nft::TimerArg& arg);
        void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
    private:
        int _attr_type;
};

#endif
