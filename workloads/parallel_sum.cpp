#include <iostream>
#include <vector>
#include <thread>
#include <numeric>

void partial_sum(const std::vector<uint64_t>& data, size_t start, size_t end, uint64_t& result) {
    uint64_t s = 0;
    for (size_t i = start; i < end; ++i) {
        s += data[i];
    }
    result = s;
}

int main(int argc, char* argv[]) {
    size_t n = 100000;
    if (argc > 1) {
        try { n = std::stoull(argv[1]); } catch (...) {}
    }

    std::vector<uint64_t> data(n);
    std::iota(data.begin(), data.end(), 1);

    unsigned int num_threads = 4;
    std::vector<std::thread> threads;
    std::vector<uint64_t> results(num_threads, 0);

    size_t chunk = n / num_threads;
    for (unsigned int i = 0; i < num_threads; ++i) {
        size_t start = i * chunk;
        size_t end = (i == num_threads - 1) ? n : (i + 1) * chunk;
        threads.emplace_back(partial_sum, std::cref(data), start, end, std::ref(results[i]));
    }

    for (auto& t : threads) {
        t.join();
    }

    uint64_t total = 0;
    for (uint64_t r : results) total += r;

    std::cout << "[WORKLOAD:parallel_sum] Sum of 1 to " << n << " = " << total << "\n";
    return 0;
}
