#include <Common/ThreadProfileEvents.h>

#if defined(__linux__)
#include <unistd.h>
#include <linux/perf_event.h>
#include <syscall.h>
#include <sys/ioctl.h>
#include <cerrno>
#include "hasLinuxCapability.h"
#endif

namespace DB
{

#if defined(__linux__)

    static PerfEventInfo softwareEvent(int event_config, ProfileEvents::Event profile_event)
    {
        return PerfEventInfo
        {
            .event_type = perf_type_id::PERF_TYPE_SOFTWARE,
            .event_config = event_config,
            .profile_event = profile_event
        };
    }

    static PerfEventInfo hardwareEvent(int event_config, ProfileEvents::Event profile_event)
    {
        return PerfEventInfo
        {
            .event_type = perf_type_id::PERF_TYPE_HARDWARE,
            .event_config = event_config,
            .profile_event = profile_event
        };
    }

    // descriptions' source: http://man7.org/linux/man-pages/man2/perf_event_open.2.html
    const PerfEventInfo PerfEventsCounters::raw_events_info[] = {
            hardwareEvent(PERF_COUNT_HW_CPU_CYCLES, ProfileEvents::PERF_COUNT_HW_CPU_CYCLES),
            hardwareEvent(PERF_COUNT_HW_INSTRUCTIONS, ProfileEvents::PERF_COUNT_HW_INSTRUCTIONS),
            hardwareEvent(PERF_COUNT_HW_CACHE_REFERENCES, ProfileEvents::PERF_COUNT_HW_CACHE_REFERENCES),
            hardwareEvent(PERF_COUNT_HW_CACHE_MISSES, ProfileEvents::PERF_COUNT_HW_CACHE_MISSES),
            hardwareEvent(PERF_COUNT_HW_BRANCH_INSTRUCTIONS, ProfileEvents::PERF_COUNT_HW_BRANCH_INSTRUCTIONS),
            hardwareEvent(PERF_COUNT_HW_BRANCH_MISSES, ProfileEvents::PERF_COUNT_HW_BRANCH_MISSES),
            hardwareEvent(PERF_COUNT_HW_BUS_CYCLES, ProfileEvents::PERF_COUNT_HW_BUS_CYCLES),
            hardwareEvent(PERF_COUNT_HW_STALLED_CYCLES_FRONTEND, ProfileEvents::PERF_COUNT_HW_STALLED_CYCLES_FRONTEND),
            hardwareEvent(PERF_COUNT_HW_STALLED_CYCLES_BACKEND, ProfileEvents::PERF_COUNT_HW_STALLED_CYCLES_BACKEND),
            hardwareEvent(PERF_COUNT_HW_REF_CPU_CYCLES, ProfileEvents::PERF_COUNT_HW_REF_CPU_CYCLES),
            // This reports the CPU clock, a high-resolution per-CPU timer.
            // a bit broken according to this: https://stackoverflow.com/a/56967896
//            softwareEvent(PERF_COUNT_SW_CPU_CLOCK, ProfileEvents::PERF_COUNT_SW_CPU_CLOCK),
            softwareEvent(PERF_COUNT_SW_TASK_CLOCK, ProfileEvents::PERF_COUNT_SW_TASK_CLOCK),
            softwareEvent(PERF_COUNT_SW_PAGE_FAULTS, ProfileEvents::PERF_COUNT_SW_PAGE_FAULTS),
            softwareEvent(PERF_COUNT_SW_CONTEXT_SWITCHES, ProfileEvents::PERF_COUNT_SW_CONTEXT_SWITCHES),
            softwareEvent(PERF_COUNT_SW_CPU_MIGRATIONS, ProfileEvents::PERF_COUNT_SW_CPU_MIGRATIONS),
            softwareEvent(PERF_COUNT_SW_PAGE_FAULTS_MIN, ProfileEvents::PERF_COUNT_SW_PAGE_FAULTS_MIN),
            softwareEvent(PERF_COUNT_SW_PAGE_FAULTS_MAJ, ProfileEvents::PERF_COUNT_SW_PAGE_FAULTS_MAJ),
            softwareEvent(PERF_COUNT_SW_ALIGNMENT_FAULTS, ProfileEvents::PERF_COUNT_SW_ALIGNMENT_FAULTS),
            softwareEvent(PERF_COUNT_SW_EMULATION_FAULTS, ProfileEvents::PERF_COUNT_SW_EMULATION_FAULTS)
            // This is a placeholder event that counts nothing. Informational sample record types such as mmap or
            // comm must be associated with an active event. This dummy event allows gathering such records
            // without requiring a counting event.
//            softwareEventInfo(PERF_COUNT_SW_DUMMY, ProfileEvents::PERF_COUNT_SW_DUMMY)
    };
    static_assert(std::size(PerfEventsCounters::raw_events_info) == PerfEventsCounters::NUMBER_OF_RAW_EVENTS);

    thread_local PerfDescriptorsHolder PerfEventsCounters::thread_events_descriptors_holder{};
    thread_local bool PerfEventsCounters::thread_events_descriptors_opened = false;
    thread_local PerfEventsCounters * PerfEventsCounters::current_thread_counters = nullptr;

    std::atomic<bool> PerfEventsCounters::perf_unavailability_logged = false;
    std::atomic<bool> PerfEventsCounters::particular_events_unavailability_logged = false;

    Logger * PerfEventsCounters::getLogger()
    {
        return &Logger::get("PerfEventsCounters");
    }

    UInt64 PerfEventsCounters::getRawValue(int event_type, int event_config) const
    {
        for (size_t i = 0; i < NUMBER_OF_RAW_EVENTS; ++i)
        {
            const PerfEventInfo & event_info = raw_events_info[i];
            if (event_info.event_type == event_type && event_info.event_config == event_config)
                return raw_event_values[i];
        }

        LOG_WARNING(getLogger(), "Can't find perf event info for event_type=" << event_type << ", event_config=" << event_config);
        return 0;
    }

    static int openPerfEvent(perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, UInt64 flags)
    {
        return static_cast<int>(syscall(SYS_perf_event_open, hw_event, pid, cpu, group_fd, flags));
    }

    static bool getPerfEventParanoid(Int32 & result)
    {
        // the longest possible variant: "-1\0"
        constexpr Int32 max_length = 3;

        FILE * fp = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
        if (fp == nullptr)
            return false;

        char str[max_length];
        char * res = fgets(str, max_length, fp);
        fclose(fp);
        if (res == nullptr)
            return false;

        str[max_length - 1] = '\0';
        Int64 value = strtol(str, nullptr, 10);
        // the only way to be incorrect is to not be a number
        if (value == 0 && errno != 0)
            return false;

        result = static_cast<Int32>(value);
        return true;
    }

    static void perfEventOpenDisabled(Int32 perf_event_paranoid, bool has_cap_sys_admin, int perf_event_type, int perf_event_config, int & event_file_descriptor)
    {
        perf_event_attr pe = perf_event_attr();
        pe.type = perf_event_type;
        pe.size = sizeof(struct perf_event_attr);
        pe.config = perf_event_config;
        // disable by default to add as little extra time as possible
        pe.disabled = 1;
        // can record kernel only when `perf_event_paranoid` <= 1 or have CAP_SYS_ADMIN
        pe.exclude_kernel = perf_event_paranoid >= 2 && !has_cap_sys_admin;

        event_file_descriptor = openPerfEvent(&pe, /* measure the calling thread */ 0, /* on any cpu */ -1, -1, 0);
    }

    bool PerfEventsCounters::initializeThreadLocalEvents(PerfEventsCounters & counters)
    {
        if (thread_events_descriptors_opened)
            return true;

        Int32 perf_event_paranoid = 0;
        bool is_pref_available = getPerfEventParanoid(perf_event_paranoid);
        if (!is_pref_available)
        {
            bool expected_value = false;
            if (perf_unavailability_logged.compare_exchange_strong(expected_value, true))
                LOG_INFO(getLogger(), "Perf events are unsupported");
            return false;
        }

        bool has_cap_sys_admin = hasLinuxCapability(CAP_SYS_ADMIN);
        if (perf_event_paranoid >= 3 && !has_cap_sys_admin)
        {
            bool expected_value = false;
            if (perf_unavailability_logged.compare_exchange_strong(expected_value, true))
                LOG_INFO(getLogger(), "Not enough permissions to record perf events");
            return false;
        }

        bool expected = false;
        bool log_unsupported_event = particular_events_unavailability_logged.compare_exchange_strong(expected, true);
        for (size_t i = 0; i < NUMBER_OF_RAW_EVENTS; ++i)
        {
            counters.raw_event_values[i] = 0;
            const PerfEventInfo & event_info = raw_events_info[i];
            int & fd = thread_events_descriptors_holder.descriptors[i];
            perfEventOpenDisabled(perf_event_paranoid, has_cap_sys_admin, event_info.event_type, event_info.event_config, fd);

            if (fd == -1 && log_unsupported_event)
            {
                LOG_INFO(getLogger(), "Perf event is unsupported: event_type=" << event_info.event_type
                                                                               << ", event_config=" << event_info.event_config);
            }
        }

        thread_events_descriptors_opened = true;
        return true;
    }

    void PerfEventsCounters::initializeProfileEvents(PerfEventsCounters & counters)
    {
        if (current_thread_counters == &counters)
            return;
        if (current_thread_counters != nullptr)
        {
            LOG_WARNING(getLogger(), "Only one instance of `PerfEventsCounters` can be used on the thread");
            return;
        }

        if (!initializeThreadLocalEvents(counters))
            return;

        for (UInt64 & raw_value : counters.raw_event_values)
            raw_value = 0;

        for (int fd : thread_events_descriptors_holder.descriptors)
        {
            if (fd != -1)
                ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
        }

        current_thread_counters = &counters;
    }

    void PerfEventsCounters::finalizeProfileEvents(PerfEventsCounters & counters, ProfileEvents::Counters & profile_events)
    {
        if (current_thread_counters != &counters)
            return;
        if (!thread_events_descriptors_opened)
            return;

        // process raw events

        // only read counters here to have as little overhead for processing as possible
        for (size_t i = 0; i < NUMBER_OF_RAW_EVENTS; ++i)
        {
            int fd = counters.thread_events_descriptors_holder.descriptors[i];
            if (fd == -1)
                continue;

            constexpr ssize_t bytes_to_read = sizeof(counters.raw_event_values[0]);
            if (read(fd, &counters.raw_event_values[i], bytes_to_read) != bytes_to_read)
            {
                LOG_WARNING(getLogger(), "Can't read event value from file descriptor: " << fd);
                counters.raw_event_values[i] = 0;
            }
        }

        // actually process counters' values and stop measuring
        for (size_t i = 0; i < NUMBER_OF_RAW_EVENTS; ++i)
        {
            int fd = counters.thread_events_descriptors_holder.descriptors[i];
            if (fd == -1)
                continue;

            profile_events.increment(raw_events_info[i].profile_event, counters.raw_event_values[i]);

            if (ioctl(fd, PERF_EVENT_IOC_DISABLE, 0))
                LOG_WARNING(getLogger(), "Can't disable perf event with file descriptor: " << fd);
            if (ioctl(fd, PERF_EVENT_IOC_RESET, 0))
                LOG_WARNING(getLogger(), "Can't reset perf event with file descriptor: " << fd);
        }

        // process custom events which depend on the raw ones
        UInt64 hw_cpu_cycles = counters.getRawValue(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES);
        UInt64 hw_ref_cpu_cycles = counters.getRawValue(PERF_TYPE_HARDWARE, PERF_COUNT_HW_REF_CPU_CYCLES);

        UInt64 instructions_per_cpu_scaled = hw_cpu_cycles != 0
                ? counters.getRawValue(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS) / hw_cpu_cycles
                : 0;
        UInt64 instructions_per_cpu = hw_ref_cpu_cycles != 0
                ? counters.getRawValue(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS) / hw_ref_cpu_cycles
                : 0;

        profile_events.increment(ProfileEvents::PERF_CUSTOM_INSTRUCTIONS_PER_CPU_CYCLE_SCALED, instructions_per_cpu_scaled);
        profile_events.increment(ProfileEvents::PERF_CUSTOM_INSTRUCTIONS_PER_CPU_CYCLE, instructions_per_cpu);

        current_thread_counters = nullptr;
    }

    Logger * PerfDescriptorsHolder::getLogger()
    {
        return &Logger::get("PerfDescriptorsHolder");
    }

    PerfDescriptorsHolder::PerfDescriptorsHolder()
    {
        for (int & descriptor : descriptors)
            descriptor = -1;
    }

    PerfDescriptorsHolder::~PerfDescriptorsHolder()
    {
        for (int & descriptor : descriptors)
        {
            if (descriptor == -1)
                continue;

            if (ioctl(descriptor, PERF_EVENT_IOC_DISABLE, 0))
                LOG_WARNING(getLogger(), "Can't disable perf event with file descriptor: " << descriptor);
            if (close(descriptor))
                LOG_WARNING(getLogger(),"Can't close perf event file descriptor: " << descriptor
                                                                                   << "; error: " << errno << " - " << strerror(errno));

            descriptor = -1;
        }
    }
#else

    void PerfEventsCounters::initializeProfileEvents(PerfEventsCounters &) {}
    void PerfEventsCounters::finalizeProfileEvents(PerfEventsCounters &, ProfileEvents::Counters &) {}

#endif

}