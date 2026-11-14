#include "process_trace.hpp"

#include <windows.h>

#include <evntcons.h>
#include <evntrace.h>

#include <string>
#include <vector>

#include "houseguest/system/pipe.hpp"

namespace houseguest::service {
namespace {

constexpr std::uint16_t kProcessStartEventId = 1;
constexpr ULONGLONG kProcessKeyword = 0x10;
constexpr ULONG kSystemTimeClock = 2;
constexpr GUID kKernelProcessProvider = {
    0x22fb2cd6, 0x0e7b, 0x422b, {0xa0, 0xc7, 0x2f, 0xad, 0x1f, 0xd0, 0xe7, 0x16}};

const std::wstring& session_name() {
    static const std::wstring name = L"HouseguestProcessTrace" + system::instance_suffix();
    return name;
}

std::vector<std::uint8_t> properties_buffer() {
    std::vector<std::uint8_t> buffer(sizeof(EVENT_TRACE_PROPERTIES) +
                                     (session_name().size() + 1) * sizeof(wchar_t));
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    properties->Wnode.BufferSize = static_cast<ULONG>(buffer.size());
    properties->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    properties->Wnode.ClientContext = kSystemTimeClock;
    properties->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    properties->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    return buffer;
}

void WINAPI callback(PEVENT_RECORD record) {
    ProcessTrace::deliver(record);
}

void stop_session(TRACEHANDLE session) {
    auto buffer = properties_buffer();
    ControlTraceW(session, session == 0 ? session_name().c_str() : nullptr,
                  reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data()), EVENT_TRACE_CONTROL_STOP);
}

}  // namespace

ProcessTrace::ProcessTrace(Handler handler) : handler_(std::move(handler)) {}

ProcessTrace::~ProcessTrace() {
    stop();
}

bool ProcessTrace::start(std::string& error) {
    if (alive_) return true;
    stop();

    auto buffer = properties_buffer();
    auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buffer.data());
    TRACEHANDLE session = 0;
    ULONG status = StartTraceW(&session, session_name().c_str(), properties);
    if (status == ERROR_ALREADY_EXISTS) {
        stop_session(0);
        status = StartTraceW(&session, session_name().c_str(), properties);
    }
    if (status != ERROR_SUCCESS) {
        error = "StartTrace failed with code " + std::to_string(status);
        return false;
    }

    status = EnableTraceEx2(session, &kKernelProcessProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION, kProcessKeyword, 0, 0, nullptr);
    if (status != ERROR_SUCCESS) {
        error = "EnableTraceEx2 failed with code " + std::to_string(status);
        stop_session(session);
        return false;
    }

    EVENT_TRACE_LOGFILEW log{};
    log.LoggerName = const_cast<LPWSTR>(session_name().c_str());
    log.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    log.EventRecordCallback = callback;
    log.Context = this;
    const TRACEHANDLE consumer = OpenTraceW(&log);
    if (consumer == INVALID_PROCESSTRACE_HANDLE) {
        error = "OpenTrace failed with code " + std::to_string(GetLastError());
        stop_session(session);
        return false;
    }

    session_ = session;
    consumer_ = consumer;
    alive_ = true;
    thread_ = std::thread([this, consumer] {
        TRACEHANDLE handle = consumer;
        ::ProcessTrace(&handle, 1, nullptr, nullptr);
        alive_ = false;
    });
    return true;
}

void ProcessTrace::stop() {
    if (session_ != 0) stop_session(session_);
    if (thread_.joinable()) thread_.join();
    if (consumer_ != 0) CloseTrace(consumer_);
    session_ = 0;
    consumer_ = 0;
    alive_ = false;
}

void ProcessTrace::deliver(void* raw) {
    const auto* record = static_cast<const EVENT_RECORD*>(raw);
    auto* self = static_cast<ProcessTrace*>(record->UserContext);
    if (!self || record->EventHeader.EventDescriptor.Id != kProcessStartEventId ||
        !IsEqualGUID(record->EventHeader.ProviderId, kKernelProcessProvider) ||
        record->UserDataLength < sizeof(std::uint32_t)) {
        return;
    }
    self->handler_(*static_cast<const std::uint32_t*>(record->UserData),
                   static_cast<std::uint64_t>(record->EventHeader.TimeStamp.QuadPart));
}

}  // namespace houseguest::service
