#include <iostream>

#include "../SyncedChunkedArray.h"

int main() {
    using List = SyncedChunkedArray<int>;
    List list;

    for(int i=0;i<4000;i++)list.emplace(i);

    List::trackable_iterator two_iter = list.emplace(2)();

    auto fn = [&](){
        list.iterate([&](auto iter){
            if (*iter > 500){
                list.erase(iter);
            } else {
                (*iter)++;
            }
        });
    };

    std::thread t1(fn);
    std::thread t2(fn);
    t1.join();
    t2.join();

    std::cout << *two_iter.lock();    // Output: 4

    return 0;
}