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

void test_trackable_iterator_move(){
    using List = SyncedChunkedArray<int>;
    List list;

    for(int i=0;i<40000;i++)list.emplace(i);

    List::trackable_iterator two_iter = list.emplace(2)();
    List::trackable_iterator other_iter;

    auto fn = [&](){
        list.iterate([&](auto iter){
            /*if (*iter > 500){
                list.erase(iter);
            } else {
                (*iter)++;
            }*/

            other_iter = {iter};
        });

        std::cout << "iterate end" << std::endl;
    };

    std::thread t1(fn);
    std::thread t2(fn);
    t1.join();
    t2.join();


    std::cout << "----" << std::endl;

    std::cout << *two_iter.lock() << std::endl;    // Output: 4
    std::cout << *other_iter.lock() << std::endl;    // Output: 4

}

int main() {

    //reuse_test().run();
    benchmark_iterate(10000);
    //test_trackable_iterator_erase();
    //test_trackable_iterator_move();

    return 0;
}