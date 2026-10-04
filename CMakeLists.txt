cmake_minimum_required(VERSION 3.16)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/dist.info" _tdlua_dist_version_line
     REGEX "^version[ \t]*=")
string(REGEX MATCH "\"([0-9]+\\.[0-9]+)\""
       _tdlua_dist_version_match "${_tdlua_dist_version_line}")
if(NOT _tdlua_dist_version_match)
  message(FATAL_ERROR "Unable to read TDLua version from dist.info")
endif()
set(_tdlua_base_version "${CMAKE_MATCH_1}")
project(tdlua VERSION "${_tdlua_base_version}.0" LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(TDLUA_BASE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
option(TDLUA_BUILD_TESTS "Build TDLua regression tests" ON)
option(TDLUA_BUNDLED_TDLIB "Build the TDLib copy in the td subdirectory" ON)
option(TDLUA_TD_STATIC "Link TDLua against the static TDLib JSON library" OFF)
set(TDLUA_TDLIB_VERSION "")
set(TDLUA_LUA_VERSION "" CACHE STRING "Lua version used for the module installation path")
set(TDLUA_LUA_MODULE_DIR "" CACHE STRING "Installation directory for the tdlua Lua module")
include(GNUInstallDirs)

# TDLib has its own CTest and benchmark targets. TDLua controls its tests
# below, so keep the vendored dependency from adding its full test tree.
set(BUILD_TESTING OFF CACHE BOOL "Disable TDLib tests when building TDLua" FORCE)

if(TDLUA_LUA_VERSION AND LUA_INCLUDE_DIR AND LUA_LIBRARY)
  # Older CMake FindLua modules do not recognize Lua 5.5 headers. When the
  # caller supplies an explicit Lua version and paths, use those paths
  # directly and add the libraries needed by a static Lua build.
  set(LUA_VERSION_STRING "${TDLUA_LUA_VERSION}")
  set(LUA_INCLUDE_DIRS "${LUA_INCLUDE_DIR}")
  set(LUA_LIBRARIES "${LUA_LIBRARY}")
  if(UNIX AND NOT APPLE AND NOT BEOS)
    find_library(LUA_MATH_LIBRARY m)
    if(LUA_MATH_LIBRARY)
      list(APPEND LUA_LIBRARIES "${LUA_MATH_LIBRARY}")
    endif()
    if(LUA_LIBRARY MATCHES "\\.a$" AND CMAKE_DL_LIBS)
      list(APPEND LUA_LIBRARIES "${CMAKE_DL_LIBS}")
    endif()
  endif()
elseif(TDLUA_LUA_VERSION)
  find_package(Lua ${TDLUA_LUA_VERSION} EXACT REQUIRED)
else()
  find_package(Lua REQUIRED)
endif()

if(TDLUA_BUNDLED_TDLIB)
  set(TD_INSTALL_STATIC_LIBRARIES OFF CACHE BOOL "" FORCE)
  set(TD_INSTALL_SHARED_LIBRARIES ON CACHE BOOL "" FORCE)
  add_subdirectory(td)
else()
  find_package(Td CONFIG REQUIRED)
endif()

if(TDLUA_TDLIB_VERSION STREQUAL "")
  if(TDLUA_BUNDLED_TDLIB AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/td/CMakeLists.txt")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/td/CMakeLists.txt" _tdlib_project_line
         REGEX "^project\\(TDLib VERSION ")
    string(REGEX MATCH "VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)"
           _tdlib_project_match "${_tdlib_project_line}")
    if(_tdlib_project_match)
      set(TDLUA_TDLIB_VERSION "${CMAKE_MATCH_1}")
    endif()
  elseif(DEFINED TDLib_VERSION)
    set(TDLUA_TDLIB_VERSION "${TDLib_VERSION}")
  elseif(DEFINED Td_VERSION)
    set(TDLUA_TDLIB_VERSION "${Td_VERSION}")
  elseif(DEFINED Td_DIR AND EXISTS "${Td_DIR}/TdConfigVersion.cmake")
    file(STRINGS "${Td_DIR}/TdConfigVersion.cmake" _tdlib_version_line
         REGEX "^set\\(PACKAGE_VERSION ")
    string(REGEX MATCH "\"([0-9]+\\.[0-9]+\\.[0-9]+)\""
           _tdlib_version_match "${_tdlib_version_line}")
    if(_tdlib_version_match)
      set(TDLUA_TDLIB_VERSION "${CMAKE_MATCH_1}")
    endif()
  endif()
endif()
if(TDLUA_TDLIB_VERSION STREQUAL "")
  set(TDLUA_TDLIB_VERSION "unknown")
endif()
set(TDLUA_VERSION_STRING "${TDLUA_BASE_VERSION}-${TDLUA_TDLIB_VERSION}")
message(STATUS "TDLua version: ${TDLUA_VERSION_STRING}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/tdlua-version.txt"
     "${TDLUA_VERSION_STRING}\n")

list(APPEND tdlua_srcs
    "src/tdlua.cpp"
    "src/luajson.cpp"
    "src/luaIF.cpp"
    "src/request_router.cpp"
    "src/lua_dispatcher.cpp"
)
if (TDLUA_CALLS)
    set(TARGET "libtgvoip")
    list(APPEND libtgvoip__c_srcs
        "libtgvoip/webrtc_dsp/modules/third_party/fft/fft.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/pitch_estimator.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/lpc_shape_swb16_tables.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/pitch_gain_tables.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/arith_routines_logist.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/filterbanks.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/transform.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/pitch_filter.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/encode_lpc_swb.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/filter_functions.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/decode.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/lattice.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/intialize.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/lpc_tables.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/lpc_gain_swb_tables.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/bandwidth_estimator.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/encode.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/lpc_analysis.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/arith_routines_hist.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/entropy_coding.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/isac_vad.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/arith_routines.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/crc.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/lpc_shape_swb12_tables.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/decode_bwe.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/spectrum_ar_model_tables.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/pitch_lag_tables.c"
        "libtgvoip/webrtc_dsp/modules/audio_coding/codecs/isac/main/source/isac.c"
        "libtgvoip/webrtc_dsp/modules/audio_processing/ns/nsx_core.c"
        "libtgvoip/webrtc_dsp/modules/audio_processing/ns/noise_suppression_x.c"
        "libtgvoip/webrtc_dsp/modules/audio_processing/ns/nsx_core_c.c"
        "libtgvoip/webrtc_dsp/modules/audio_processing/ns/ns_core.c"
        "libtgvoip/webrtc_dsp/modules/audio_processing/ns/noise_suppression.c"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc/legacy/analog_agc.c"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc/legacy/digital_agc.c"
        "libtgvoip/webrtc_dsp/common_audio/third_party/fft4g/fft4g.c"
        "libtgvoip/webrtc_dsp/common_audio/ring_buffer.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/complex_fft.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/filter_ma_fast_q12.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/splitting_filter1.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/levinson_durbin.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/dot_product_with_scale.cc"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/auto_corr_to_refl_coef.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/resample_by_2_internal.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/energy.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/sqrt_of_one_minus_x_squared.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/downsample_fast.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/filter_ar_fast_q12.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/spl_init.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/lpc_to_refl_coef.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/cross_correlation.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/division_operations.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/auto_correlation.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/get_scaling_square.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/resample.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/min_max_operations.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/refl_coef_to_lpc.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/filter_ar.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/vector_scaling_operations.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/resample_fractional.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/real_fft.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/ilbc_specific_functions.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/randomization_functions.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/copy_set_operations.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/resample_by_2.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/get_hanning_window.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/resample_48khz.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/spl_inl.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/spl_sqrt.c"
        "libtgvoip/webrtc_dsp/common_audio/vad/vad_sp.c"
        "libtgvoip/webrtc_dsp/common_audio/vad/vad.cc"
        "libtgvoip/webrtc_dsp/common_audio/vad/webrtc_vad.c"
        "libtgvoip/webrtc_dsp/common_audio/vad/vad_filterbank.c"
        "libtgvoip/webrtc_dsp/common_audio/vad/vad_core.c"
        "libtgvoip/webrtc_dsp/common_audio/vad/vad_gmm.c"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/complex_bit_reverse.c"
        "libtgvoip/webrtc_dsp/common_audio/third_party/spl_sqrt_floor/spl_sqrt_floor.c"

    )
    list(APPEND libtgvoip__cxx_srcs
        "libtgvoip/json11.cpp"
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
        "libtgvoip/webrtc_dsp/system_wrappers/source/field_trial.cc"
        "libtgvoip/webrtc_dsp/system_wrappers/source/metrics.cc"
        "libtgvoip/webrtc_dsp/system_wrappers/source/cpu_features.cc"
        "libtgvoip/webrtc_dsp/absl/strings/internal/memutil.cc"
        "libtgvoip/webrtc_dsp/absl/strings/string_view.cc"
        "libtgvoip/webrtc_dsp/absl/strings/ascii.cc"
        "libtgvoip/webrtc_dsp/absl/types/bad_optional_access.cc"
        "libtgvoip/webrtc_dsp/absl/types/optional.cc"
        "libtgvoip/webrtc_dsp/absl/base/internal/raw_logging.cc"
        "libtgvoip/webrtc_dsp/absl/base/internal/throw_delegate.cc"
        "libtgvoip/webrtc_dsp/rtc_base/race_checker.cc"
        "libtgvoip/webrtc_dsp/rtc_base/strings/string_builder.cc"
        "libtgvoip/webrtc_dsp/rtc_base/memory/aligned_malloc.cc"
        "libtgvoip/webrtc_dsp/rtc_base/timeutils.cc"
        "libtgvoip/webrtc_dsp/rtc_base/platform_file.cc"
        "libtgvoip/webrtc_dsp/rtc_base/string_to_number.cc"
        "libtgvoip/webrtc_dsp/rtc_base/thread_checker_impl.cc"
        "libtgvoip/webrtc_dsp/rtc_base/stringencode.cc"
        "libtgvoip/webrtc_dsp/rtc_base/stringutils.cc"
        "libtgvoip/webrtc_dsp/rtc_base/checks.cc"
        "libtgvoip/webrtc_dsp/rtc_base/platform_thread.cc"
        "libtgvoip/webrtc_dsp/rtc_base/logging_webrtc.cc"
        "libtgvoip/webrtc_dsp/rtc_base/criticalsection.cc"
        "libtgvoip/webrtc_dsp/rtc_base/platform_thread_types.cc"
        "libtgvoip/webrtc_dsp/rtc_base/event.cc"
        "libtgvoip/webrtc_dsp/rtc_base/event_tracer.cc"
        "libtgvoip/webrtc_dsp/third_party/rnnoise/src/rnn_vad_weights.cc"
        "libtgvoip/webrtc_dsp/third_party/rnnoise/src/kiss_fft.cc"
        "libtgvoip/webrtc_dsp/api/audio/audio_frame.cc"
        "libtgvoip/webrtc_dsp/api/audio/echo_canceller3_config.cc"
        "libtgvoip/webrtc_dsp/api/audio/echo_canceller3_factory.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/rms_level.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/echo_detector/normalized_covariance_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/echo_detector/moving_max.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/echo_detector/circular_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/echo_detector/mean_variance_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/splitting_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/gain_control_impl.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/audio_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/typing_detection.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/include/audio_processing_statistics.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/include/audio_generator_factory.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/include/aec_dump.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/include/audio_processing.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/include/config.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/interpolated_gain_curve.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/agc2_common.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/gain_applier.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/adaptive_agc.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/adaptive_digital_gain_applier.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/limiter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/saturation_protector.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/spectral_features_internal.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/rnn.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/pitch_search_internal.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/spectral_features.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/pitch_search.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/features_extraction.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/fft_util.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/rnn_vad/lp_residual.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/adaptive_mode_level_estimator_agc.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/vector_float_frame.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/noise_level_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/agc2_testing_common.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/fixed_digital_level_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/fixed_gain_controller.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/vad_with_level.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/limiter_db_gain_curve.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/down_sampler.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/signal_classifier.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/noise_spectrum_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/compute_interpolated_gain_curve.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/biquad_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc2/adaptive_mode_level_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/transient/moving_moments.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/transient/wpd_tree.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/transient/wpd_node.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/transient/transient_suppressor.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/transient/transient_detector.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/low_cut_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/level_estimator_impl.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/three_band_filter_bank.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec/echo_cancellation.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec/aec_resampler.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec/aec_core.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec/aec_core_sse2.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/voice_detection_impl.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/echo_cancellation_impl.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/gain_control_for_experimental_agc.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc/agc.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc/loudness_histogram.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc/agc_manager_direct.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/agc/utility.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/audio_processing_impl.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/audio_generator/file_audio_generator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/gain_controller2.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/residual_echo_detector.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/noise_suppression_impl.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aecm/aecm_core.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aecm/aecm_core_c.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aecm/echo_control_mobile.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_reverb_model.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/reverb_model_fallback.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/echo_remover_metrics.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/matched_filter_lag_aggregator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_delay_buffer2.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/echo_path_variability.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/frame_blocker.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/utility/ooura_fft_sse2.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/subtractor.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/aec3_fft.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/fullband_erle_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/suppression_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/block_processor.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/subband_erle_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_delay_controller_metrics.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_delay_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/vector_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/erl_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/aec_state.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/adaptive_fir_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_delay_controller.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/skew_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/echo_path_delay_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/block_framer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/erle_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/reverb_model.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/cascaded_biquad_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/subtractor_output.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/stationarity_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_signal_analyzer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/subtractor_output_analyzer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/suppression_gain.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/echo_audibility.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/block_processor_metrics.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/moving_average.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/reverb_model_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/aec3_common.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/residual_echo_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/matched_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/reverb_decay_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/render_delay_controller2.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/suppression_gain_limiter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/main_filter_update_gain.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/echo_remover.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/downsampled_render_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/matrix_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/block_processor2.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/echo_canceller3.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/block_delay_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/fft_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/comfort_noise_generator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/shadow_filter_update_gain.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/filter_analyzer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/reverb_frequency_response.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/aec3/decimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/echo_control_mobile_impl.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/logging/apm_data_dumper.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/voice_activity_detector.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/standalone_vad.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/pitch_internal.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/vad_circular_buffer.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/vad_audio_proc.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/pole_zero_filter.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/pitch_based_vad.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/vad/gmm.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/utility/ooura_fft.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/utility/delay_estimator_wrapper.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/utility/delay_estimator.cc"
        "libtgvoip/webrtc_dsp/modules/audio_processing/utility/block_mean_calculator.cc"
        "libtgvoip/webrtc_dsp/common_audio/window_generator.cc"
        "libtgvoip/webrtc_dsp/common_audio/channel_buffer.cc"
        "libtgvoip/webrtc_dsp/common_audio/fir_filter_factory.cc"
        "libtgvoip/webrtc_dsp/common_audio/wav_header.cc"
        "libtgvoip/webrtc_dsp/common_audio/real_fourier_ooura.cc"
        "libtgvoip/webrtc_dsp/common_audio/audio_util.cc"
        "libtgvoip/webrtc_dsp/common_audio/fir_filter_sse.cc"
        "libtgvoip/webrtc_dsp/common_audio/resampler/push_sinc_resampler.cc"
        "libtgvoip/webrtc_dsp/common_audio/resampler/resampler.cc"
        "libtgvoip/webrtc_dsp/common_audio/resampler/sinc_resampler_sse.cc"
        "libtgvoip/webrtc_dsp/common_audio/resampler/push_resampler.cc"
        "libtgvoip/webrtc_dsp/common_audio/resampler/sinc_resampler.cc"
        "libtgvoip/webrtc_dsp/common_audio/resampler/sinusoidal_linear_chirp_source.cc"
        "libtgvoip/webrtc_dsp/common_audio/wav_file.cc"
        "libtgvoip/webrtc_dsp/common_audio/audio_converter.cc"
        "libtgvoip/webrtc_dsp/common_audio/real_fourier.cc"
        "libtgvoip/webrtc_dsp/common_audio/sparse_fir_filter.cc"
        "libtgvoip/webrtc_dsp/common_audio/smoothing_filter.cc"
        "libtgvoip/webrtc_dsp/common_audio/fir_filter_c.cc"
        "libtgvoip/webrtc_dsp/common_audio/signal_processing/dot_product_with_scale.cc"
        "libtgvoip/webrtc_dsp/common_audio/vad/vad.cc"
        "libtgvoip/video/VideoSource.cpp"
        "libtgvoip/video/VideoRenderer.cpp"
        "libtgvoip/video/ScreamCongestionController.cpp"
    )
    include_directories (
        "includes"
        "libtgvoip/webrtc_dsp"
    )
    add_library(libtgvoip STATIC ${libtgvoip__c_srcs} ${libtgvoip__cxx_srcs})
    set_target_properties(libtgvoip PROPERTIES PREFIX "lib")
    set_target_properties(libtgvoip PROPERTIES ARCHIVE_OUTPUT_NAME "tgvoip")
    set_target_properties(libtgvoip PROPERTIES SUFFIX ".a")
    set_target_properties(libtgvoip PROPERTIES COMPILE_DEFINITIONS "WEBRTC_APM_DEBUG_DUMP=0;TGVOIP_USE_DESKTOP_DSP;WEBRTC_POSIX;WEBRTC_LINUX;WEBRTC_NS_FLOAT;USE_KISS_FFT;FIXED_POINT;TGVOIP_USE_CXX11_LIB;TGVOIP_OTHER;TGVOIP_USE_CALLBACK_AUDIO_IO;TGVOIP_LOG_VERBOSITY=3")
    target_link_libraries (libtgvoip
        opus
    #    "libopus.a"
    #    "/usr/local/lib/libopus.a"
        pthread
        stdc++
        crypto
        ssl
        dl
        "-pthread"
    #    "-Wl,--allow-shlib-undefined"
    )
    set_property (TARGET libtgvoip PROPERTY CXX_STANDARD 11)

    list(APPEND tdlua_srcs
        "base64/base64.cpp"
        "src/LuaTDVoip.cpp"
    )
endif()

add_library (tdlua SHARED ${tdlua_srcs})

target_include_directories(tdlua PRIVATE
    "${LUA_INCLUDE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/includes"
)
target_link_libraries(tdlua PRIVATE ${LUA_LIBRARIES})
target_compile_features(tdlua PRIVATE cxx_std_11)
target_compile_definitions(tdlua PRIVATE
    "TDLUA_VERSION_STRING=\"${TDLUA_VERSION_STRING}\""
    "TDLUA_BASE_VERSION=\"${TDLUA_BASE_VERSION}\""
    "TDLUA_TDLIB_VERSION=\"${TDLUA_TDLIB_VERSION}\"")

if(TDLUA_CALLS)
    target_compile_definitions(tdlua PRIVATE
        TGVOIP_USE_CALLBACK_AUDIO_IO WEBRTC_POSIX TDLUA_CALLS)
    target_link_libraries (tdlua PRIVATE libtgvoip)
endif()

set_target_properties(tdlua PROPERTIES PREFIX "")
if(TDLUA_TD_STATIC)
  target_link_libraries (tdlua PRIVATE Td::TdJsonStatic)
else()
  target_link_libraries (tdlua PRIVATE Td::TdJson)
endif()
if(TDLUA_BUNDLED_TDLIB AND UNIX)
  if(APPLE)
    set_property(TARGET tdlua APPEND PROPERTY INSTALL_RPATH
        "@loader_path;@loader_path/../..")
  else()
    set_property(TARGET tdlua APPEND PROPERTY INSTALL_RPATH
        "$ORIGIN;$ORIGIN/../..")
  endif()
endif()

if(NOT TDLUA_LUA_VERSION)
  if(LUA_VERSION_STRING)
    string(REGEX MATCH "^[0-9]+\\.[0-9]+" TDLUA_LUA_VERSION_MATCH
           "${LUA_VERSION_STRING}")
    if(TDLUA_LUA_VERSION_MATCH)
      set(TDLUA_LUA_VERSION "${TDLUA_LUA_VERSION_MATCH}")
    else()
      set(TDLUA_LUA_VERSION "${LUA_VERSION_STRING}")
    endif()
  else()
    string(REGEX MATCH "lua([0-9]+\\.[0-9]+)" TDLUA_LUA_VERSION_MATCH
           "${LUA_INCLUDE_DIR}")
    if(CMAKE_MATCH_1)
      set(TDLUA_LUA_VERSION "${CMAKE_MATCH_1}")
    else()
      set(TDLUA_LUA_VERSION "generic")
    endif()
  endif()
endif()
if(TDLUA_LUA_MODULE_DIR STREQUAL "")
    set(TDLUA_LUA_MODULE_DIR
        "${CMAKE_INSTALL_LIBDIR}/lua/${TDLUA_LUA_VERSION}"
        CACHE STRING "Installation directory for the tdlua Lua module" FORCE)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tdluaConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tdlua"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(TARGETS tdlua
    EXPORT TDLuaTargets
    LIBRARY DESTINATION "${TDLUA_LUA_MODULE_DIR}"
    RUNTIME DESTINATION "${TDLUA_LUA_MODULE_DIR}"
)
install(EXPORT TDLuaTargets
    FILE TDLuaTargets.cmake
    NAMESPACE TDLua::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tdlua"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/tdluaConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/tdlua"
)

if(TDLUA_BUILD_TESTS)
    enable_testing()
    add_executable(tdlua_codec_test
        "tests/codec_test.cpp"
        "src/luajson.cpp"
    )
    target_include_directories(tdlua_codec_test PRIVATE
        ${LUA_INCLUDE_DIR}
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        ${CMAKE_CURRENT_SOURCE_DIR}
        "${CMAKE_CURRENT_SOURCE_DIR}/includes"
    )
    target_link_libraries(tdlua_codec_test PRIVATE ${LUA_LIBRARIES})
    set_property(TARGET tdlua_codec_test PROPERTY CXX_STANDARD 11)
    add_test(NAME tdlua_codec COMMAND tdlua_codec_test)

    if(LUA_EXECUTABLE)
        set(TDLUA_LUA_EXECUTABLE "${LUA_EXECUTABLE}")
    else()
        find_program(TDLUA_LUA_EXECUTABLE NAMES lua5.3 lua5.2 lua5.4 lua)
    endif()
    if(TDLUA_LUA_EXECUTABLE)
        add_test(NAME tdlua_lifecycle
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/lifecycle.lua"
                    "$<TARGET_FILE_DIR:tdlua>"
        )
        add_test(NAME tdlua_async
            COMMAND ${TDLUA_LUA_EXECUTABLE}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tests/async.lua"
                    "$<TARGET_FILE_DIR:tdlua>"
        )
    endif()
endif()
