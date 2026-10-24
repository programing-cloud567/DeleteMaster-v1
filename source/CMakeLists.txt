cmake_minimum_required(VERSION 3.20)
project(DeleteMaster LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(NOT MSVC)
    message(FATAL_ERROR "DeleteMaster собирается только компилятором MSVC")
endif()

set(DM_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(DM_OUT ${CMAKE_BINARY_DIR}/bin)
set(DM_ENGINES_OUT ${DM_OUT}/engines)

set(DM_WARN /W3)
set(DM_DEFS /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX)

function(dm_set_output target outdir)
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${outdir}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${outdir}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${outdir}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${outdir}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${outdir}"
        PREFIX "")
endfunction()

function(dm_engine target outname source)
    add_library(${target} SHARED ${source})
    target_include_directories(${target} PRIVATE ${DM_ROOT}/include)
    target_compile_options(${target} PRIVATE /utf-8 /EHsc ${DM_WARN} ${DM_DEFS})
    target_compile_definitions(${target} PRIVATE DM_ENGINE_BUILD=1)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${outname}")
    target_link_libraries(${target} PRIVATE advapi32 user32 shell32 ole32 rpcrt4 rstrtmgr)
    dm_set_output(${target} "${DM_ENGINES_OUT}")
endfunction()

dm_engine(eng_basic     "DeleteEngine.Basic"     src/engines/basic/basic.cpp)
dm_engine(eng_scan      "DeleteEngine.Scan"      src/engines/scan/scan.cpp)
dm_engine(eng_freeze    "DeleteEngine.Freeze"    src/engines/freeze/freeze.cpp)
dm_engine(eng_kill      "DeleteEngine.Kill"      src/engines/kill/kill.cpp)
dm_engine(eng_service   "DeleteEngine.Service"   src/engines/service/service.cpp)
dm_engine(eng_privilege "DeleteEngine.Privilege" src/engines/privilege/privilege.cpp)
dm_engine(eng_acl       "DeleteEngine.Acl"       src/engines/acl/acl.cpp)
dm_engine(eng_nulldacl  "DeleteEngine.Nulldacl"  src/engines/nulldacl/nulldacl.cpp)
dm_engine(eng_native    "DeleteEngine.Native"    src/engines/native/native.cpp)
dm_engine(eng_deleteonclose "DeleteEngine.Deleteonclose" src/engines/deleteonclose/deleteonclose.cpp)
dm_engine(eng_globalroot "DeleteEngine.Globalroot" src/engines/globalroot/globalroot.cpp)
dm_engine(eng_staging   "DeleteEngine.Staging"   src/engines/staging/staging.cpp)
dm_engine(eng_streams   "DeleteEngine.Streams"   src/engines/streams/streams.cpp)
dm_engine(eng_reparse   "DeleteEngine.Reparse"   src/engines/reparse/reparse.cpp)
dm_engine(eng_reboot    "DeleteEngine.Reboot"    src/engines/reboot/reboot.cpp)
dm_engine(eng_squat     "DeleteEngine.Squat"     src/engines/squat/squat.cpp)
dm_engine(eng_verify    "DeleteEngine.Verify"    src/engines/verify/verify.cpp)

add_executable(DeleteMaster src/host/main.cpp src/host/app.rc)
target_include_directories(DeleteMaster PRIVATE ${DM_ROOT}/include)
target_compile_options(DeleteMaster PRIVATE /utf-8 /EHsc ${DM_WARN} ${DM_DEFS})
target_link_libraries(DeleteMaster PRIVATE advapi32 user32 shell32 ole32 oleaut32 taskschd)
dm_set_output(DeleteMaster "${DM_OUT}")

add_dependencies(DeleteMaster eng_basic eng_scan eng_freeze eng_kill eng_nulldacl eng_deleteonclose eng_globalroot eng_squat eng_service eng_privilege eng_acl
                              eng_native eng_staging eng_streams eng_reparse eng_reboot eng_verify)

add_custom_command(TARGET DeleteMaster POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DM_ENGINES_OUT}"
    COMMENT "engines directory ready")


