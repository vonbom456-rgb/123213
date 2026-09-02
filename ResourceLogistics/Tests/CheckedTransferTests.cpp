#include "../Source/CheckedTransfer.h"
#include <iostream>
#include <stdexcept>
#include <climits>
void check(bool ok) { if(!ok) throw std::runtime_error("test failed"); }
int main() {
    int tests=0;
    for(int available:{0,1,10,10000}) for(int requested:{0,1,10,2000}) for(int capacity:{0,1,5,10000}) {
        int source=available,destination=0;
        int moved=StorageSafety::CheckedTransfer(requested,[&]{return source;},[&]{return destination;},
            [&](int delta){source+=delta;},[&](int delta){destination+=std::min(delta,capacity);});
        check(source+destination==available); check(moved==destination); check(moved<=1000); ++tests;
    }
    for(int partial:{0,3}) {
        int source=20,destination=0; bool threw=false;
        try { StorageSafety::CheckedTransfer(10,[&]{return source;},[&]{return destination;},
            [&](int d){source+=d;},[&](int){destination+=partial;throw std::runtime_error("callback");}); }
        catch(...) {threw=true;}
        check(threw && source+destination==20 && source==20-partial); ++tests;
    }
    {
        int src=20,dst=0; bool threw=false;
        try { StorageSafety::CheckedTransfer(10,[&]{return src;},[&]{return dst;},
            [&](int d){if(d<0)src+=d;},[&](int){}); } catch(...) {threw=true;}
        check(threw); ++tests; // Failed rollback must fault, never report success.
    }
    {
        int src=20,dst=INT_MAX; bool changed=false;
        check(StorageSafety::CheckedTransfer(10,[&]{return src;},[&]{return dst;},
            [&](int){changed=true;},[&](int){changed=true;})==0 && !changed); ++tests;
    }
    {
        int src=20,dst=0;
        check(StorageSafety::CheckedTransfer(10,[&]{return src;},[&]{return dst;},
            [&](int){},[&](int d){dst+=d;})==0 && src==20 && dst==0); ++tests;
    }
    std::cout<<tests<<" transfer accounting tests passed\n";
}
