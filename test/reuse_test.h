#ifndef SYNCCHUNKEDARRAY_REUSE_TEST_H
#define SYNCCHUNKEDARRAY_REUSE_TEST_H

#include <numeric>
#include <cmath>
#include "../SyncedChunkedArray.h"

struct reuse_test{
    void run(){
        SyncedChunkedArray<int, 4> list;

        const int size = 4*20;
        for(int i=0;i<size;i++){
            list.emplace(i);
        }

        auto show = [&](){
            std::cout << "----" << std::endl;
            list.iterate([&](auto &&iter) {
                std::cout << *iter << std::endl;
            });
        };

        auto show_sum = [&](){
            std::size_t sum = 0;
            list.iterate([&](auto &&iter) {
                sum += *iter;
            });
            std::cout << "sum = " << sum << std::endl;
        };
        std::cout << "=====" << std::endl;
        std::cout << "chunks " << list.get_chunks_count() << std::endl;
        show_sum();
        //show();


        // erase
        for (int k=0;k<1;k++) {
            std::vector<int> erased;
            const float erase_probability = 70;
            const int break_point = std::round(RAND_MAX * erase_probability / 100.0);

            int i = 0;
            list.iterate([&](auto &&iter) {
                const int random_variable = std::rand();
                if (random_variable < break_point) {
                    list.erase(iter);
                    erased.emplace_back(*iter);
                }

                i++;
            });
            std::cout << "=====" << std::endl;
            //show();
            std::cout << "erased = " << std::accumulate(erased.begin(), erased.end(), 0) << std::endl;
            show_sum();
            std::cout << "chunks " << list.get_chunks_count() << std::endl;


            // emplace
            for (int i : erased) {
                list.emplace(i);
            }

            std::cout << "=====" << std::endl;
            show_sum();
            std::cout << "chunks " << list.get_chunks_count() << std::endl;
            //show();
        }


    }
};

#endif //SYNCCHUNKEDARRAY_REUSE_TEST_H
