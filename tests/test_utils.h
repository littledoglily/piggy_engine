#pragma once
#include <iostream>
#include <cassert>

#define TEST(name) do { std::cout << "[TEST] " #name "... "; } while(0)
#define PASS()     do { std::cout << "PASS\n"; } while(0)
#define FAIL(msg)  do { std::cout << "FAIL: " << msg << "\n"; assert(false); } while(0)
