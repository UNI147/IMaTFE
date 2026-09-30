#pragma once
#include <fstream>
#include <ostream>
#include <string_view>
#include <iostream>
namespace imatfe::tests { class TestLog { public: static TestLog& instance(){static TestLog x;return x;} template<class T> TestLog& operator<<(const T& v){std::cout<<v; if(file_)file_<<v; return *this;} TestLog& operator<<(std::ostream&(*m)(std::ostream&)){m(std::cout);if(file_)m(file_);return *this;} private: TestLog():file_("imatfe_tests.log"){} std::ofstream file_; }; }
