#
# Copyright Soramitsu Co., Ltd. All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0
#

add_library(memory_profiler
        memory_profiler.cpp
        )
target_link_libraries(memory_profiler
        fmt::fmt
        )
