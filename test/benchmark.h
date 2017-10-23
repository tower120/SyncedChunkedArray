#pragma once

#include <chrono>
#include <list>
#include <array>
#include <vector>
#include <iostream>
#include <cmath>
#include <thread>
#include <deque>
#include "../SyncedChunkedArray.h"

template<class time_unit = std::chrono::milliseconds, class Closure>
auto measure(Closure&& closure){
    using namespace std::chrono;
    high_resolution_clock::time_point t1 = high_resolution_clock::now();
    closure();
    high_resolution_clock::time_point t2 = high_resolution_clock::now();

    return duration_cast<time_unit>( t2 - t1 ).count();
}


template<class time_unit = std::chrono::milliseconds, class Closure>
auto benchmark(int times, Closure&& closure){
    return measure<time_unit>([&](){
        for(int i=0;i<times;i++)
            closure();
    });
}

struct BigData {
    long long value;

    BigData(long long value)
        :value(value)
    {}

    //std::array<char, 1> payload;
};


void benchmark_iterate(int times) {
    const int size = 1000;
    static constexpr const int threads_count = 4;

    SyncedChunkedArray<BigData> arr;       // 512 for benchmark
    std::vector<BigData> vec;

    {
        auto t = measure([&]() {
            for (int i = 0; i < size; i++)
                vec.emplace_back(i);
        });

        std::cout << "vec inserted in: " << t << std::endl;
    }

    {
        auto t = measure([&]() {
            for (int i = 0; i < size; i++)
                arr.emplace(i);
        });

        std::cout << "arr inserted in: " << t << std::endl;
    }

    // random erase
    {
        const float erase_probability = 50;        // 50 - worst case
        const int break_point = std::round(RAND_MAX * erase_probability / 100.0);
        auto t = measure([&]() {
            std::size_t i = 0;
            arr.iterate([&](auto &&iter) {
                const int random_variable = std::rand();
                if (random_variable < break_point) {
                    arr.erase(iter);
                    //vec.erase(vec.begin() + i);
                    vec.pop_back();
                    i--;
                }
                i++;

            });
        });
        std::cout << "Erased in: " << t << std::endl;
    }

    auto benchmark_threaded_read = [&](auto times, auto&& closure) -> std::size_t {
        std::array<std::thread, threads_count> threads;

        std::atomic<std::size_t> total{0};

        if (threads_count == 0){
            return benchmark(times, [&]() {
                closure();
            });
        }
        for (int i = 0; i < threads_count; i++) {
            threads[i] = std::thread([&](){
                auto t = benchmark(times, [&]() {
                    closure();
                });
                total.fetch_add(t, std::memory_order_relaxed);
            });
        }
        for (auto &thread : threads) thread.join();

        return total.load();
    };


    {
        std::size_t sum{0};
        std::size_t iterate_count{0};

        auto t = benchmark_threaded_read(times, [&]() {
            for(auto& i : vec) {
                //i.i++;

                iterate_count++;
                //sum += i();
                //if (!i.first) continue;
                sum += i.value;
                //i.i--;
            }
        });

        std::cout << "vec: " << t << " [" << sum << "] it: "  <<  iterate_count << std::endl;
    }


    {
        std::size_t sum{0};
        std::size_t iterate_count{0};
        auto t = benchmark_threaded_read(times, [&]() {
            /*auto r = arr.range();
            auto iter = ForIterator(r);
            for(auto i : iter) {
                sum += *i;
            }*/
            //arr.iterate([](auto &&) {});
            arr.iterate_shared([&](const auto &i) {
                iterate_count++;
                //sum += i();
                sum += (*i).value;
            });
        });

        std::cout << "ChunkedArray: " << t << " [" << sum << "] it: " <<  iterate_count << std::endl;
    }

}