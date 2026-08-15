#include <fstream>
#include <iostream>
#include <random>
#include <string>

std::string random_key(std::mt19937& rng, int id) {
    // "u" + unique number ensures that we don't accidentally
    // overwrite an existing key.
    return "u" + std::to_string(id);
}

int main() {
    constexpr int NUM_KV = 10000;

    std::ofstream out("insert_commands.txt");

    if (!out) {
        std::cerr << "Failed to create insert_commands.txt\n";
        return 1;
    }

    std::random_device rd;
    std::mt19937 rng(rd());

    std::uniform_int_distribution<int> value_dist(1, 1000000);

    for (int i = 1; i <= NUM_KV; ++i) {
        std::string key = random_key(rng, i);
        int value = value_dist(rng);

        out << "put " << key << " " << value << "\n";
    }

    out << "pages\n";
    out << "scan \"\" 100\n";
    out << "exit\n";

    out.close();

    std::cout << "Generated " << NUM_KV
              << " KV insert commands.\n";

    return 0;
}