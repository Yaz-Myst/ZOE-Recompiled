cmake_minimum_required(VERSION 3.21)

project(ps2xIOP LANGUAGES CXX)

option(PS2X_IOP_BUILD_TESTS "Build ps2xIOP emulator smoke tests" OFF)

add_library(ps2_iop STATIC
    src/ps2_path.cpp
    src/iop_module_manager.cpp
    src/iop_subsystem.cpp
    src/emulator/iop_emulator.cpp
    src/emulator/core/iop_cpu.cpp
    src/emulator/core/iop_kernel.cpp
    src/emulator/core/iop_memory.cpp
    src/emulator/services/iop_module_loader.cpp
    src/emulator/services/iop_rpc.cpp
    src/emulator/imports/iop_cdvd.cpp
    src/emulator/imports/iop_heaplib.cpp
    src/emulator/imports/iop_imports.cpp
    src/emulator/imports/iop_intrman.cpp
    src/emulator/imports/iop_ioman.cpp
    src/emulator/imports/iop_loadcore.cpp
    src/emulator/imports/iop_stdio.cpp
    src/emulator/imports/iop_sysclib.cpp
    src/emulator/imports/iop_sysmem.cpp
    src/emulator/imports/iop_timrman.cpp
    src/emulator/imports/iop_vblank.cpp
    src/modules/dbcman.cpp
    src/modules/libsd.cpp
    src/modules/mcserv.cpp
)

target_compile_features(ps2_iop PUBLIC cxx_std_20)

# SPU2 (sound processor): PCSX2's SPU2 core (GPL-3.0+), kept close to upstream in its own library so
# its PCSX2-style include layout (Config.h, SPU2/defs.h, ...) stays private. See src/emulator/spu2/README.md.
add_library(ps2_iop_spu2 STATIC
    src/emulator/spu2/spu2_device.cpp
    src/emulator/spu2/pcsx2/SPU2/ADSR.cpp
    src/emulator/spu2/pcsx2/SPU2/Dma.cpp
    src/emulator/spu2/pcsx2/SPU2/Mixer.cpp
    src/emulator/spu2/pcsx2/SPU2/ReadInput.cpp
    src/emulator/spu2/pcsx2/SPU2/RegTable.cpp
    src/emulator/spu2/pcsx2/SPU2/Reverb.cpp
    src/emulator/spu2/pcsx2/SPU2/ReverbResample.cpp
    src/emulator/spu2/pcsx2/SPU2/spu2sys.cpp
)
target_compile_features(ps2_iop_spu2 PUBLIC cxx_std_20)
target_include_directories(ps2_iop_spu2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/emulator/spu2/pcsx2
)
if(MSVC)
    # PCSX2 code: silence its benign conversion/unused warnings rather than editing upstream files.
    target_compile_options(ps2_iop_spu2 PRIVATE /W1 /FIPrecompiledHeader.h)
else()
    target_compile_options(ps2_iop_spu2 PRIVATE -w -mssse3 -include PrecompiledHeader.h)
endif()
target_link_libraries(ps2_iop PRIVATE ps2_iop_spu2)

target_include_directories(ps2_iop
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

add_library(ps2x::iop ALIAS ps2_iop)

if(PS2X_IOP_BUILD_TESTS)
    enable_testing()
    add_executable(ps2_iop_emulator_tests tests/iop_emulator_tests.cpp)
    target_link_libraries(ps2_iop_emulator_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_emulator_tests COMMAND ps2_iop_emulator_tests)

    add_executable(ps2_iop_import_tests tests/iop_import_tests.cpp)
    target_link_libraries(ps2_iop_import_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_tests COMMAND ps2_iop_import_tests)

    add_executable(ps2_iop_compatibility_tests tests/iop_compatibility_tests.cpp)
    target_link_libraries(ps2_iop_compatibility_tests PRIVATE ps2_iop)
    add_test(NAME ps2_iop_compatibility_tests COMMAND ps2_iop_compatibility_tests)

    add_executable(ps2_iop_import_version_tests tests/iop_import_version_tests.cpp)
    target_link_libraries(ps2_iop_import_version_tests PRIVATE ps2_iop)
    target_include_directories(ps2_iop_import_version_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    add_test(NAME ps2_iop_import_version_tests COMMAND ps2_iop_import_version_tests)

endif()

install(TARGETS ps2_iop
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/
    DESTINATION include
)
