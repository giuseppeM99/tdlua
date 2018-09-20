project (tdlua)
cmake_minimum_required (VERSION 3.1)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(Td REQUIRED)
if(NOT LUA_INCLUDE_DIR)
  find_package(Lua REQUIRED)
endif()
include_directories(${LUA_INCLUDE_DIR})

set(TARGET "libtgvoip")
list(APPEND libtgvoip__c_srcs
    "libtgvoip/webrtc_dsp/webrtc/common_audio/fft4g.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/ring_buffer.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/auto_corr_to_refl_coef.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/auto_correlation.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/complex_bit_reverse.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/complex_fft.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/copy_set_operations.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/cross_correlation.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/division_operations.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/dot_product_with_scale.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/downsample_fast.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/energy.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/filter_ar.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/filter_ar_fast_q12.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/filter_ma_fast_q12.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/get_hanning_window.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/get_scaling_square.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/ilbc_specific_functions.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/levinson_durbin.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/lpc_to_refl_coef.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/min_max_operations.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/randomization_functions.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/real_fft.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/refl_coef_to_lpc.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/resample.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/resample_48khz.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/resample_by_2.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/resample_by_2_internal.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/resample_fractional.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/spl_init.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/spl_inl.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/spl_sqrt.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/spl_sqrt_floor.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/splitting_filter_impl.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/sqrt_of_one_minus_x_squared.c"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/signal_processing/vector_scaling_operations.c"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/ns/noise_suppression_x.c"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/ns/nsx_core_c.c"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/ns/nsx_core.c"
)
list(APPEND libtgvoip__cxx_srcs
    "libtgvoip/VoIPController.cpp"
    "libtgvoip/Buffers.cpp"
    "libtgvoip/CongestionControl.cpp"
    "libtgvoip/EchoCanceller.cpp"
    "libtgvoip/JitterBuffer.cpp"
    "libtgvoip/logging.cpp"
    "libtgvoip/MediaStreamItf.cpp"
    "libtgvoip/MessageThread.cpp"
    "libtgvoip/NetworkSocket.cpp"
    "libtgvoip/OpusDecoder.cpp"
    "libtgvoip/OpusEncoder.cpp"
    "libtgvoip/PacketReassembler.cpp"
    "libtgvoip/VoIPGroupController.cpp"
    "libtgvoip/VoIPServerConfig.cpp"
    "libtgvoip/audio/AudioInput.cpp"
    "libtgvoip/audio/AudioOutput.cpp"
    "libtgvoip/audio/AudioIOCallback.cpp"
    "libtgvoip/audio/AudioIO.cpp"
    "libtgvoip/audio/Resampler.cpp"
    "libtgvoip/os/posix/NetworkSocketPosix.cpp"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/sparse_fir_filter.cc"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/wav_file.cc"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/wav_header.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/aec/aec_core.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/aec/aec_core_sse2.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/aec/aec_resampler.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/aec/echo_cancellation.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/aecm/aecm_core.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/aecm/aecm_core_c.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/aecm/echo_control_mobile.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/agc/legacy/analog_agc.c"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/agc/legacy/digital_agc.c"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/logging/apm_data_dumper.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/splitting_filter.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/three_band_filter_bank.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/utility/block_mean_calculator.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/utility/delay_estimator.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/utility/delay_estimator_wrapper.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/utility/ooura_fft.cc"
    "libtgvoip/webrtc_dsp/webrtc/modules/audio_processing/utility/ooura_fft_sse2.cc"
    "libtgvoip/webrtc_dsp/webrtc/system_wrappers/source/cpu_features.cc"
    "libtgvoip/webrtc_dsp/webrtc/base/checks.cc"
    "libtgvoip/webrtc_dsp/webrtc/base/stringutils.cc"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/audio_util.cc"
    "libtgvoip/webrtc_dsp/webrtc/common_audio/channel_buffer.cc"
)
include_directories (
    "includes"
    "libtgvoip/audio/"
#    "libtgvoip/webrtc_dsp"
)
add_library(libtgvoip STATIC ${libtgvoip__c_srcs} ${libtgvoip__cxx_srcs})
set_target_properties(libtgvoip PROPERTIES PREFIX "lib")
set_target_properties(libtgvoip PROPERTIES ARCHIVE_OUTPUT_NAME "tgvoip")
set_target_properties(libtgvoip PROPERTIES SUFFIX ".a")
list(APPEND libtgvoip__include_dirs  "${CMAKE_CURRENT_LIST_DIR}/libtgvoip/webrtc_dsp")
set_property(TARGET libtgvoip APPEND PROPERTY INCLUDE_DIRECTORIES ${libtgvoip__include_dirs})
set_target_properties(libtgvoip PROPERTIES COMPILE_DEFINITIONS "WEBRTC_APM_DEBUG_DUMP=0;TGVOIP_USE_DESKTOP_DSP;WEBRTC_POSIX;USE_KISS_FFT;FIXED_POINT;TGVOIP_USE_CXX11_LIB;TGVOIP_OTHER;TGVOIP_USE_CALLBACK_AUDIO_IO;TGVOIP_LOG_VERBOSITY=0")
target_link_libraries (libtgvoip
#    opus
#    "libopus.a"
    "/usr/local/lib/libopus.a"
    pthread
    stdc++
    crypto
    ssl
    dl
    "-pthread"
    "-Wl,--allow-shlib-undefined"
)
set_property (TARGET libtgvoip PROPERTY CXX_STANDARD 11)

#include_directories (libtgvoip)
add_library (tdlua SHARED tdlua.cpp luajson.cpp LuaTDVoip.cpp base64/base64.cpp)

set_target_properties(tdlua PROPERTIES PREFIX "")
set_target_properties(tdlua PROPERTIES COMPILE_DEFINITIONS "TGVOIP_USE_CALLBACK_AUDIO_IO")
if(TDLUA_TD_STATIC)
  target_link_libraries (tdlua PRIVATE Td::TdJsonStatic libtgvoip)
else()
  target_link_libraries (tdlua PRIVATE Td::TdJson libtgvoip)
endif()
set_property (TARGET tdlua PROPERTY CXX_STANDARD 11)
