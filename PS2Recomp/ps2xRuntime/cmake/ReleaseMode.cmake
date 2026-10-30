include(CheckIPOSupported)

# Link-time optimization makes a 37,000-file recompiled game very slow to build and is not needed:
# the hot code is already compiled with -O3. MSVC keeps its /GL + /LTCG pair (Windows behaviour is
# unchanged); GCC/Clang enable IPO only when asked. build-linux.sh leaves this off by default.
option(PS2X_ENABLE_LTO "Enable link-time optimization for GCC/Clang builds" OFF)

check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)

function(EnableFastReleaseMode TargetName)
    message("> Enabling optimization for: ${TargetName}")
    if(MSVC)
        target_compile_options(${TargetName} PRIVATE
            $<$<CONFIG:Release>:
                /O2 # speed
                /Ob2 # inline aggressively
                /Oi # intrinsics
                /GL # whole program opt
                /Gy # function-level linking
                /Gw # global data in COMDAT
                /GF # string pooling
                /Zc:inline # remove unreferenced inline
                /fp:fast # fast math (graphics friendly)
                /DNDEBUG
                /arch:AVX2 # Advanced Vector Extensions 2
                /GS- # Disable Buffer Security Check (faster)
                /Qspectre- # Disable Spectre mitigations (faster)
            >
        )

        if(TARGET ${TargetName})
            target_link_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:
                    /LTCG # link-time code generation
                    /OPT:REF # remove unreferenced
                    /OPT:ICF # fold identical COMDATs
                >
            )
        endif()
    endif()

    if(NOT MSVC)
        # GCC/Clang: match the Windows build's /arch:AVX2 baseline. The runtime uses SSE4.1
        # intrinsics (_mm_extract_epi32/_mm_extract_epi64) that x86-64 does not imply.
        # Steam Deck (Zen 2) and every desktop CPU of the last decade support AVX2.
        # Configure with -DPS2X_PORTABLE_CPU=ON for a plain x86-64-v2 build.
        option(PS2X_PORTABLE_CPU "Build for x86-64-v2 instead of AVX2 (older CPUs, slower)" OFF)
        if(PS2X_PORTABLE_CPU)
            target_compile_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:-O3;-msse4.2;-mpopcnt;-DNDEBUG>
            )
        else()
            target_compile_options(${TargetName} PRIVATE
                $<$<CONFIG:Release>:-O3;-mavx2;-mfma;-msse4.2;-DNDEBUG>
            )
        endif()
    endif()

    if(MSVC OR PS2X_ENABLE_LTO)
        if(IPO_SUPPORTED)
            set_property(TARGET ${TargetName} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        else()
            message(WARNING "Interprocedural optimization not supported: ${IPO_ERROR}")
        endif()
    endif()
endfunction()