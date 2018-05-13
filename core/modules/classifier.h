#ifndef CLASSIFIER_H
#define CLASSIFIER_H


#include "../module.h"  //继承module类
#include "../pb/nft_msg.pb.h"    //protoc生成的protobuf消息解析支持

class Classifier final : public Module {
    public:
        Classifier(): Module(),counter(0){}
        
        //static const Commands cmds;

        CommandResponse Init(const bess::nft::ClassifierArg &arg);

        void ProcessBatch(Context* ctx, bess::PacketBatch* batch) override;
    private:
        //第一个版本没有设置classify的接口，分类规则直接在classify中编码
        bool classify(bess::Packet* pkt);
	
	int counter;
};
#endif
