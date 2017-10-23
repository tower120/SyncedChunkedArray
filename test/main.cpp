#include <iostream>
#include "../SyncedChunkedArray.h"
//#include "../v2/SyncedChunkedArray.h"

#include "benchmark.h"
#include "reuse_test.h"


void test_trackable_iterator_erase(){
    SyncedChunkedArray<int, 4> list;

    auto show = [&](){
        list.iterate([&](auto&& iter){
            std::cout << *iter << std::endl;
        });
    };

    for(int i=0; i<15;i++){
        list.emplace(i);
    }
    auto iter = list.emplace(-1)();
    show();

   // auto ptr = iter.lock();     // keep locked

    static bool f{true};
    list.iterate([&](auto&& iter){
        if (f) list.erase(iter);
       // f = false;
    });

//    list.iterate([&](auto&& iter){});

    std::cout << "erased" << std::endl;

    {
        auto ptr = iter.lock();     // keep locked
        if (!ptr){
            std::cout << "ptr dead" << std::endl;
        } else {
            std::cout << *ptr << std::endl;
        }
    }

    show();

}

int main() {

    //reuse_test().run();
    benchmark_iterate(10000);
    //test_trackable_iterator_erase();

    return 0;
}