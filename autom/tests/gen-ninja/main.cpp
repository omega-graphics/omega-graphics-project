#include <iostream>
#include "mathlib.h"
#include "utils.h"

int main() {
    std::cout << "Version: " << get_version() << std::endl;
    std::cout << "2 + 3 = " << add(2, 3) << std::endl;
    std::cout << "4 * 5 = " << multiply(4, 5) << std::endl;
    return (add(2, 3) == 5 && multiply(4, 5) == 20) ? 0 : 1;
}
