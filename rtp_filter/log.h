#ifndef __LOG_H__
#define __LOG_H__
#include <sstream>
#include <string.h>
#include <string>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <iomanip>

#define __FILENAME__ (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)

class LogHelp
{
   private:
#ifdef __MACH__
    static pid_t gettid() { return pthread_mach_thread_np(pthread_self()); }
#else
    static pid_t gettid() { return static_cast<pid_t>(::syscall(SYS_gettid)); }
#endif

   public:
    LogHelp(const char *f, int l, const char *v) : f(f), line(l)
    {
        ss << format_time() << " " << gettid() << " " << v << " ";
    }
    ~LogHelp()
    {
        ss << " " << f << ":" << line;
        printf("%s\n", ss.str().data());
    }
    template <typename T>
    LogHelp &operator<<(T t)
    {
        ss << t;
        return *this;
    }

   private:
    static std::string format_time()
    {
        char buf[64] = {0};
        struct timeval tv;
        gettimeofday(&tv, nullptr);
        time_t seconds = tv.tv_sec;
        long microseconds = tv.tv_usec;
        struct tm tm_time;
        ::gmtime_r(&seconds, &tm_time);
        snprintf(buf,
                 sizeof(buf),
                 "%4d%02d%02d %02d:%02d:%02d.%06ld",
                 tm_time.tm_year + 1900,
                 tm_time.tm_mon + 1,
                 tm_time.tm_mday,
                 tm_time.tm_hour,
                 tm_time.tm_min,
                 tm_time.tm_sec,
                 microseconds);
        return buf;
    }

   private:
    std::stringstream ss;
    const char *f;
    int line;
};

#define LOG_INFO LogHelp(__FILENAME__, __LINE__, "INFO")
#define LOG_DEBUG LogHelp(__FILENAME__, __LINE__, "DEBUG")
#define LOG_WAR LogHelp(__FILENAME__, __LINE__, "WARN")
#define LOG_ERROR LogHelp(__FILENAME__, __LINE__, "ERROR")

#endif
