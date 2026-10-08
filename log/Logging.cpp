#include "Logging.h"
#include "CurrentThread.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

// 为了实现多线程中日志时间格式化的效率，增加了两个__thread变量，
// 用于缓存当前线程存日期时间字符串、上一次日志记录的秒数
__thread char t_time[64];		// 当前线程的时间字符串 “年:月:日 时:分:秒”
__thread time_t t_lastsecond;	// 当前线程上一次日志记录时的秒数

// 方便一个已知长度的字符串被送入buffer中
class Template{
    public:
        Template(const char * str, unsigned len)
            : str_(str),
              len_(len){}
        const char *str_;
        const unsigned len_;
};

// 重载运算符，使LogStream可以处理Template类型的数据。
inline LogStream& operator<<(LogStream& s, Template v){
    s.append(v.str_, v.len_);
    return s;

}
// 重载运算符，使LogStream可以处理SourceFile类型的数据
inline LogStream& operator<<(LogStream& s, const Logger::SourceFile& v){
    s.append(v.data_, v.size_);
    return s;
}


Logger::SourceFile::SourceFile(const char *data) : data_(data), size_(static_cast<int>(strlen(data_)))
{
    const char *forward_slash = strrchr(data, '/');
    if (forward_slash)
    {
        data_ = forward_slash + 1;
        size_ -= static_cast<int>((data_ - data));
    }
}

Logger::Impl::Impl(Logger::LogLevel level, const Logger::SourceFile &source, int line)
    : level_(level),
      sourcefile_(source),
      line_(line){

    FormattedTime();
    CurrentThread::tid();

    stream_ << Template(CurrentThread::tidString(), CurrentThread::tidStringLength());
    stream_ << Template(loglevel(), 6);
}

void Logger::Impl::FormattedTime(){
    //格式化输出时间
    TimeStamp now = TimeStamp::Now();
    time_t seconds = static_cast<time_t>(now.microseconds() / kMicrosecond2Second);
    int microseconds = static_cast<int>(now.microseconds() % kMicrosecond2Second);

    // 变更日志记录的时间，如果不在同一秒，则更新时间。

    if (t_lastsecond != seconds)
    {
        struct tm tm_time;
        localtime_r(&seconds, &tm_time);
        snprintf(t_time, sizeof(t_time), "%4d%02d%02d %02d:%02d:%02d.",
                tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
                tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);
        t_lastsecond = seconds;
    }

    Fmt us(".%06dZ  ", microseconds);
    stream_ << Template(t_time, 17) << Template(us.data(), 9);
}

void Logger::Impl::Finish(){
    stream_ << " - " << sourcefile_.data_ << ":" << line_ << "\n";
}

LogStream &Logger::Impl::stream() { return stream_; }

const char* Logger::Impl::loglevel() const {
    switch(level_) {
        case DEBUG:
            return "DEBUG ";
        case INFO:
            return "INFO  ";
        case WARN:
            return "WARN  ";
        case ERROR:
            return "ERROR ";
        case FATAL:
            return "FATAL ";
        }   
    return nullptr;
} 


void defaultOutput(const char* msg, int len){
    fwrite(msg, 1, len, stdout);  // 默认写出到stdout
}

void defaultFlush(){
    // 强制刷新输出流缓冲区
    fflush(stdout);    // 默认flush到stdout
}

namespace {

char g_log_service[32] = "gamemesh";
thread_local uint64_t t_log_player = 0;
thread_local std::string t_log_session;
thread_local std::string t_log_error;

bool JsonLogsEnabled() {
    const char *flag = std::getenv("GAMEMESH_LOG_JSON");
    if (flag && flag[0] == '0')
        return false;
    if (flag && flag[0] == '1')
        return true;
    const char *formal = std::getenv("GAMEMESH_FORMAL");
    return formal && formal[0] == '1';
}

void AppendJsonEscaped(std::string *out, const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '"' || c == '\\') {
            out->push_back('\\');
            out->push_back(static_cast<char>(c));
        } else if (c == '\n') {
            out->append("\\n");
        } else if (c == '\r') {
            out->append("\\r");
        } else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out->append(buf);
        } else {
            out->push_back(static_cast<char>(c));
        }
    }
}

}  // namespace

void SetLogService(const char *service) {
    if (!service || !service[0])
        return;
    std::snprintf(g_log_service, sizeof(g_log_service), "%s", service);
}

void SetLogPlayer(uint64_t player_id) { t_log_player = player_id; }

void SetLogSession(const char *session_id) { t_log_session = session_id ? session_id : ""; }

void SetLogErrorCode(const char *error_code) { t_log_error = error_code ? error_code : ""; }

// 定义默认值
Logger::OutputFunc g_output = defaultOutput;
Logger::FlushFunc g_flush = defaultFlush;
Logger::LogLevel g_logLevel = Logger::LogLevel::INFO;

Logger::Logger(const char *file_, int line, Logger::LogLevel level)
    : impl_(level, file_, line){};


Logger::~Logger()
{
    impl_.Finish(); // 补足源代码位置和行数
    const LogStream::Buffer& buf(stream().buffer());  // 获取缓冲区
    if (JsonLogsEnabled()) {
        std::string line;
        line.reserve(static_cast<size_t>(buf.len()) + 128);
        line.append("{\"timestamp\":\"");
        AppendJsonEscaped(&line, t_time, std::strlen(t_time));
        line.append("\",\"level\":\"");
        const char *lv = impl_.loglevel();
        while (lv && *lv == ' ')
            ++lv;
        AppendJsonEscaped(&line, lv ? lv : "INFO", lv ? std::strlen(lv) : 4);
        // loglevel() 带尾随空格
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        line.append("\",\"service\":\"");
        AppendJsonEscaped(&line, g_log_service, std::strlen(g_log_service));
        line.append("\"");
        if (t_log_player != 0)
            line.append(",\"player_id\":").append(std::to_string(t_log_player));
        if (!t_log_session.empty()) {
            line.append(",\"session_id\":\"");
            AppendJsonEscaped(&line, t_log_session.data(), t_log_session.size());
            line.append("\"");
        }
        if (!t_log_error.empty()) {
            line.append(",\"error_code\":\"");
            AppendJsonEscaped(&line, t_log_error.data(), t_log_error.size());
            line.append("\"");
        }
        line.append(",\"msg\":\"");
        AppendJsonEscaped(&line, buf.data(), static_cast<size_t>(buf.len()));
        line.append("\"}\n");
        g_output(line.data(), static_cast<int>(line.size()));
    } else {
        g_output(buf.data(), buf.len());  // 默认输出到stdout
    }
 
    // 当日志级别为FATAL时，flush设备缓冲区并终止程序
    if (impl_.level_ == FATAL) {
        g_flush();
        abort();
    }
}


LogStream &Logger::stream() { return impl_.stream(); }

void Logger::setOutput(Logger::OutputFunc func){
    g_output = func;
}

void Logger::setFlush(Logger::FlushFunc func){
    g_flush = func;
}

void Logger::setLogLevel(Logger::LogLevel level){
    g_logLevel = level;
}