#include "classifier.h"

CommandResponse Classifier::Init(const bess::nft::ClassifierArg& arg){
    if(arg.version() != 0)
        return CommandFailure(EINVAL, "Version number must be 0");
    return CommandSuccess();
}

void Classifier::ProcessBatch(Context* ctx, bess::PacketBatch *batch){
    for(int i = 0; i < batch->cnt(); ++i){
        if(classify(batch->pkts()[i])){
            printf("packet meet needs");
        }
    }
    RunNextModule(ctx, batch);
}

bool Classifier::classify(bess::Packet* pkt){
    ++counter;
    ++pkt;
    if(counter > 300){
        counter = 0;
        return true;
    }
    return false;
}

ADD_MODULE(Classifier, "classifier", "classifier for NFT architecture") 
