cmake_minimum_required(VERSION 3.10)
project(syslogd C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# All feature flags can be toggled here
option(ENABLE_FEATURE_ROTATE_LOGFILE "Enable log file rotation" ON)
option(ENABLE_FEATURE_REMOTE_LOG "Enable remote logging" ON)
option(ENABLE_FEATURE_SYSLOGD_DUP "Enable drop duplicates" ON)
option(ENABLE_FEATURE_SYSLOGD_CFG "Enable syslog.conf support" ON)
option(ENABLE_FEATURE_IPC_SYSLOG "Enable circular buffer (IPC)" ON)
option(ENABLE_FEATURE_KMSG_SYSLOG "Enable /dev/kmsg logging" ON)

# Read buffer size in bytes (minimum 256)
set(CONFIG_FEATURE_SYSLOGD_READ_BUFFER_SIZE "256" CACHE STRING "Read buffer size")
set(CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE "16" CACHE STRING "IPC shared mem buffer size in KB")

add_executable(syslogd
    syslogd_main.c
    syslogd.c
    syslog_names.c
    compat.c
)

target_include_directories(syslogd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(syslogd PRIVATE
    CONFIG_FEATURE_SYSLOGD_READ_BUFFER_SIZE=${CONFIG_FEATURE_SYSLOGD_READ_BUFFER_SIZE}
    CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE=${CONFIG_FEATURE_IPC_SYSLOG_BUFFER_SIZE}
)

target_link_libraries(syslogd PRIVATE rt)

install(TARGETS syslogd DESTINATION sbin)
