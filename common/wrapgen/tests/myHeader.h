#include <iostream>

class TestClass {
public:

    void testFunc(){
        std::cout << "Hello World" << std::endl;
    };

    void otherFunc(int param){
        std::cout << "Hello World With Param:" << param << std::endl;
    };

}