#pragma once

#include <fstream>
#include <string>
#include <string_view>

namespace imatfe::tests
{

class TestLog final
{
public:
    static TestLog& instance()
    {
        static TestLog log;
        return log;
    }

    void line(std::string_view text)
    {
        file_ << text << '\n';
    }

    template <typename T>
    TestLog& operator<<(const T& value)
    {
        file_ << value;
        return *this;
    }

    TestLog& operator<<(std::ostream& (*manip)(std::ostream&))
    {
        file_ << manip;
        return *this;
    }

private:
    TestLog() : file_("imatfe_tests.log", std::ios::out | std::ios::trunc) {}
    std::ofstream file_;
};

} // namespace imatfe::tests
