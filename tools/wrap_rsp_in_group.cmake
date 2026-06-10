# tools/wrap_rsp_in_group.cmake
# Reads an rsp file, prepends -Wl,--start-group, appends -Wl,--end-group,
# and writes it back. This is a one-shot fix-up for a one-pass GNU ld
# that would otherwise skip libraries scanned with an empty unresolved
# set and never re-scan them when a later library pulls in symbols
# defined by the earlier one.
file(READ "${RSP_IN}" RSP_CONTENT)

string(STRIP "${RSP_CONTENT}" RSP_CONTENT)

if(RSP_CONTENT MATCHES "-Wl,--start-group")
    message(STATUS "rsp already has --start-group; leaving it alone")
    return()
endif()

set(NEW_CONTENT "-Wl,--start-group ${RSP_CONTENT} -Wl,--end-group")
file(WRITE "${RSP_OUT}" "${NEW_CONTENT}")
message(STATUS "Wrapped rsp with group flags: ${RSP_OUT}")
