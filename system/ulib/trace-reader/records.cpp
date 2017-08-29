// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <trace-reader/records.h>

#include <inttypes.h>
#include <string.h>

#include <mxtl/string_buffer.h>
#include <mxtl/string_printf.h>

namespace trace {
namespace {
const char* EventScopeToString(EventScope scope) {
    switch (scope) {
    case EventScope::kGlobal:
        return "global";
    case EventScope::kProcess:
        return "process";
    case EventScope::kThread:
        return "thread";
    }
    return "???";
}

const char* ThreadStateToString(ThreadState state) {
    switch (state) {
    case ThreadState::kNew:
        return "new";
    case ThreadState::kRunning:
        return "running";
    case ThreadState::kSuspended:
        return "suspended";
    case ThreadState::kBlocked:
        return "blocked";
    case ThreadState::kDying:
        return "dying";
    case ThreadState::kDead:
        return "dead";
    }
    return "???";
}

const char* ObjectTypeToString(mx_obj_type_t type) {
    static_assert(MX_OBJ_TYPE_LAST == 23, "need to update switch below");

    switch (type) {
    case MX_OBJ_TYPE_PROCESS:
        return "process";
    case MX_OBJ_TYPE_THREAD:
        return "thread";
    case MX_OBJ_TYPE_VMO:
        return "vmo";
    case MX_OBJ_TYPE_CHANNEL:
        return "channel";
    case MX_OBJ_TYPE_EVENT:
        return "event";
    case MX_OBJ_TYPE_PORT:
        return "port";
    case MX_OBJ_TYPE_INTERRUPT:
        return "interrupt";
    case MX_OBJ_TYPE_PCI_DEVICE:
        return "pci-device";
    case MX_OBJ_TYPE_LOG:
        return "log";
    case MX_OBJ_TYPE_SOCKET:
        return "socket";
    case MX_OBJ_TYPE_RESOURCE:
        return "resource";
    case MX_OBJ_TYPE_EVENT_PAIR:
        return "event-pair";
    case MX_OBJ_TYPE_JOB:
        return "job";
    case MX_OBJ_TYPE_VMAR:
        return "vmar";
    case MX_OBJ_TYPE_FIFO:
        return "fifo";
    case MX_OBJ_TYPE_GUEST:
        return "guest";
    case MX_OBJ_TYPE_VCPU:
        return "vcpu";
    case MX_OBJ_TYPE_TIMER:
        return "timer";
    default:
        return "???";
    }
}

mxtl::String FormatArgumentList(const mxtl::Vector<trace::Argument>& args) {
    mxtl::StringBuffer<1024> result;

    result.Append('{');
    for (size_t i = 0; i < args.size(); i++) {
        if (i != 0)
            result.Append(", ");
        result.Append(args[i].ToString());
    }
    result.Append('}');

    return result.ToString();
}
} // namespace

mxtl::String ProcessThread::ToString() const {
    return mxtl::StringPrintf("%" PRIu64 "/%" PRIu64,
                              process_koid_, thread_koid_);
}

void ArgumentValue::Destroy() {
    switch (type_) {
    case ArgumentType::kString:
        string_.~String();
        break;
    case ArgumentType::kNull:
    case ArgumentType::kInt32:
    case ArgumentType::kUint32:
    case ArgumentType::kInt64:
    case ArgumentType::kUint64:
    case ArgumentType::kDouble:
    case ArgumentType::kPointer:
    case ArgumentType::kKoid:
        break;
    }
}

void ArgumentValue::MoveFrom(ArgumentValue&& other) {
    type_ = other.type_;
    other.type_ = ArgumentType::kNull;
    switch (type_) {
    case ArgumentType::kNull:
        break;
    case ArgumentType::kInt32:
        int32_ = other.int32_;
        break;
    case ArgumentType::kUint32:
        int32_ = other.uint32_;
        break;
    case ArgumentType::kInt64:
        int64_ = other.int64_;
        break;
    case ArgumentType::kUint64:
        int64_ = other.uint64_;
        break;
    case ArgumentType::kDouble:
        double_ = other.double_;
        break;
    case ArgumentType::kString:
        new (&string_) mxtl::String(mxtl::move(other.string_));
        other.string_.~String(); // call destructor because we set other.type_ to kNull
        break;
    case ArgumentType::kPointer:
        pointer_ = other.pointer_;
        break;
    case ArgumentType::kKoid:
        koid_ = other.koid_;
        break;
    }
}

mxtl::String ArgumentValue::ToString() const {
    switch (type_) {
    case ArgumentType::kNull:
        return "null";
    case ArgumentType::kInt32:
        return mxtl::StringPrintf("int32(%" PRId32 ")", int32_);
    case ArgumentType::kUint32:
        return mxtl::StringPrintf("uint32(%" PRIu32 ")", uint32_);
    case ArgumentType::kInt64:
        return mxtl::StringPrintf("int64(%" PRId64 ")", int64_);
    case ArgumentType::kUint64:
        return mxtl::StringPrintf("uint64(%" PRIu64 ")", uint64_);
    case ArgumentType::kDouble:
        return mxtl::StringPrintf("double(%f)", double_);
    case ArgumentType::kString:
        return mxtl::StringPrintf("string(\"%s\")", string_.c_str());
    case ArgumentType::kPointer:
        return mxtl::StringPrintf("pointer(%p)", reinterpret_cast<void*>(pointer_));
    case ArgumentType::kKoid:
        return mxtl::StringPrintf("koid(%" PRIu64 ")", koid_);
    }
    MX_ASSERT(false);
}

mxtl::String Argument::ToString() const {
    return mxtl::StringPrintf("%s: %s", name_.c_str(), value_.ToString().c_str());
}

void MetadataContent::Destroy() {
    switch (type_) {
    case MetadataType::kProviderInfo:
        provider_info_.~ProviderInfo();
        break;
    case MetadataType::kProviderSection:
        provider_section_.~ProviderSection();
        break;
    }
}

void MetadataContent::MoveFrom(MetadataContent&& other) {
    type_ = other.type_;
    switch (type_) {
    case MetadataType::kProviderInfo:
        new (&provider_info_) ProviderInfo(mxtl::move(other.provider_info_));
        break;
    case MetadataType::kProviderSection:
        new (&provider_section_) ProviderSection(mxtl::move(other.provider_section_));
        break;
    }
}

mxtl::String MetadataContent::ToString() const {
    switch (type_) {
    case MetadataType::kProviderInfo:
        return mxtl::StringPrintf("ProviderInfo(id: %" PRId32 ", name: \"%s\")",
                                  provider_info_.id, provider_info_.name.c_str());
    case MetadataType::kProviderSection:
        return mxtl::StringPrintf("ProviderSection(id: %" PRId32 ")",
                                  provider_section_.id);
    }
    MX_ASSERT(false);
}

void EventData::Destroy() {
    switch (type_) {
    case EventType::kInstant:
        instant_.~Instant();
        break;
    case EventType::kCounter:
        counter_.~Counter();
        break;
    case EventType::kDurationBegin:
        duration_begin_.~DurationBegin();
        break;
    case EventType::kDurationEnd:
        duration_end_.~DurationEnd();
        break;
    case EventType::kAsyncBegin:
        async_begin_.~AsyncBegin();
        break;
    case EventType::kAsyncInstant:
        async_instant_.~AsyncInstant();
        break;
    case EventType::kAsyncEnd:
        async_end_.~AsyncEnd();
        break;
    case EventType::kFlowBegin:
        flow_begin_.~FlowBegin();
        break;
    case EventType::kFlowStep:
        flow_step_.~FlowStep();
        break;
    case EventType::kFlowEnd:
        flow_end_.~FlowEnd();
        break;
    }
}

void EventData::MoveFrom(EventData&& other) {
    type_ = other.type_;
    switch (type_) {
    case EventType::kInstant:
        new (&instant_) Instant(mxtl::move(other.instant_));
        break;
    case EventType::kCounter:
        new (&counter_) Counter(mxtl::move(other.counter_));
        break;
    case EventType::kDurationBegin:
        new (&duration_begin_) DurationBegin(mxtl::move(other.duration_begin_));
        break;
    case EventType::kDurationEnd:
        new (&duration_end_) DurationEnd(mxtl::move(other.duration_end_));
        break;
    case EventType::kAsyncBegin:
        new (&async_begin_) AsyncBegin(mxtl::move(other.async_begin_));
        break;
    case EventType::kAsyncInstant:
        new (&async_instant_) AsyncInstant(mxtl::move(other.async_instant_));
        break;
    case EventType::kAsyncEnd:
        new (&async_end_) AsyncEnd(mxtl::move(other.async_end_));
        break;
    case EventType::kFlowBegin:
        new (&flow_begin_) FlowBegin(mxtl::move(other.flow_begin_));
        break;
    case EventType::kFlowStep:
        new (&flow_step_) FlowStep(mxtl::move(other.flow_step_));
        break;
    case EventType::kFlowEnd:
        new (&flow_end_) FlowEnd(mxtl::move(other.flow_end_));
        break;
    }
}

mxtl::String EventData::ToString() const {
    switch (type_) {
    case EventType::kInstant:
        return mxtl::StringPrintf("Instant(scope: %s)",
                                  EventScopeToString(instant_.scope));
    case EventType::kCounter:
        return mxtl::StringPrintf("Counter(id: %" PRIu64 ")",
                                  counter_.id);
    case EventType::kDurationBegin:
        return "DurationBegin";
    case EventType::kDurationEnd:
        return "DurationEnd";
    case EventType::kAsyncBegin:
        return mxtl::StringPrintf("AsyncBegin(id: %" PRIu64 ")",
                                  async_begin_.id);
    case EventType::kAsyncInstant:
        return mxtl::StringPrintf("AsyncInstant(id: %" PRIu64 ")",
                                  async_instant_.id);
    case EventType::kAsyncEnd:
        return mxtl::StringPrintf("AsyncEnd(id: %" PRIu64 ")",
                                  async_end_.id);
    case EventType::kFlowBegin:
        return mxtl::StringPrintf("FlowBegin(id: %" PRIu64 ")",
                                  flow_begin_.id);
    case EventType::kFlowStep:
        return mxtl::StringPrintf("FlowStep(id: %" PRIu64 ")",
                                  flow_step_.id);
    case EventType::kFlowEnd:
        return mxtl::StringPrintf("FlowEnd(id: %" PRIu64 ")",
                                  flow_end_.id);
    }
    MX_ASSERT(false);
}

void Record::Destroy() {
    switch (type_) {
    case RecordType::kMetadata:
        metadata_.~Metadata();
        break;
    case RecordType::kInitialization:
        initialization_.~Initialization();
        break;
    case RecordType::kString:
        string_.~String();
        break;
    case RecordType::kThread:
        thread_.~Thread();
        break;
    case RecordType::kEvent:
        event_.~Event();
        break;
    case RecordType::kKernelObject:
        kernel_object_.~KernelObject();
        break;
    case RecordType::kContextSwitch:
        context_switch_.~ContextSwitch();
        break;
    case RecordType::kLog:
        log_.~Log();
        break;
    }
}

void Record::MoveFrom(Record&& other) {
    type_ = other.type_;
    switch (type_) {
    case RecordType::kMetadata:
        new (&metadata_) Metadata(mxtl::move(other.metadata_));
        break;
    case RecordType::kInitialization:
        new (&initialization_) Initialization(mxtl::move(other.initialization_));
        break;
    case RecordType::kString:
        new (&string_) String(mxtl::move(other.string_));
        break;
    case RecordType::kThread:
        new (&thread_) Thread(mxtl::move(other.thread_));
        break;
    case RecordType::kEvent:
        new (&event_) Event(mxtl::move(other.event_));
        break;
    case RecordType::kKernelObject:
        new (&kernel_object_) KernelObject(mxtl::move(other.kernel_object_));
        break;
    case RecordType::kContextSwitch:
        new (&context_switch_) ContextSwitch(mxtl::move(other.context_switch_));
        break;
    case RecordType::kLog:
        new (&log_) Log(mxtl::move(other.log_));
        break;
    }
}

mxtl::String Record::ToString() const {
    switch (type_) {
    case RecordType::kMetadata:
        return mxtl::StringPrintf("Metadata(content: %s)",
                                  metadata_.content.ToString().c_str());
    case RecordType::kInitialization:
        return mxtl::StringPrintf("Initialization(ticks_per_second: %" PRIu64 ")",
                                  initialization_.ticks_per_second);
    case RecordType::kString:
        return mxtl::StringPrintf("String(index: %" PRIu32 ", \"%s\")",
                                  string_.index, string_.string.c_str());
    case RecordType::kThread:
        return mxtl::StringPrintf("Thread(index: %" PRIu32 ", %s)",
                                  thread_.index, thread_.process_thread.ToString().c_str());
    case RecordType::kEvent:
        return mxtl::StringPrintf("Event(ts: %" PRIu64 ", pt: %s, category: \"%s\", name: \"%s\", %s, %s)",
                                  event_.timestamp, event_.process_thread.ToString().c_str(),
                                  event_.category.c_str(), event_.name.c_str(),
                                  event_.data.ToString().c_str(),
                                  FormatArgumentList(event_.arguments).c_str());
        break;
    case RecordType::kKernelObject:
        return mxtl::StringPrintf("KernelObject(koid: %" PRIu64 ", type: %s, name: \"%s\", %s)",
                                  kernel_object_.koid,
                                  ObjectTypeToString(kernel_object_.object_type),
                                  kernel_object_.name.c_str(),
                                  FormatArgumentList(kernel_object_.arguments).c_str());
        break;
    case RecordType::kContextSwitch:
        return mxtl::StringPrintf("ContextSwitch(ts: %" PRIu64 ", cpu: %" PRIu32
                                  ", os: %s, opt: %s, ipt: %s",
                                  context_switch_.timestamp,
                                  context_switch_.cpu_number,
                                  ThreadStateToString(context_switch_.outgoing_thread_state),
                                  context_switch_.outgoing_thread.ToString().c_str(),
                                  context_switch_.incoming_thread.ToString().c_str());
    case RecordType::kLog:
        return mxtl::StringPrintf("Log(ts: %" PRIu64 ", pt: %s, \"%s\")",
                                  log_.timestamp, log_.process_thread.ToString().c_str(),
                                  log_.message.c_str());
    }
    MX_ASSERT(false);
}

} // namespace trace
