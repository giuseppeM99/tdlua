# Copyright (c) 2018-2026 Giuseppe Marino
# SPDX-License-Identifier: BSD-3-Clause
"""Offline POSIX smoke for main-thread driver SIGINT and idle CPU use."""
import os
import signal
import subprocess
import sys
import time

executable, module_dir = sys.argv[1:3]
for mode in ('poll', 'loop'):
    source = "package.cpath=" + repr(module_dir + '/?.so;') + "..package.cpath; "
    source += "local t=require'tdlua'; t.setLogLevel(0); local c=t(); io.write('ready\\n'); io.flush(); "
    source += "c:poll()" if mode == 'poll' else "c:loop(function() end)"
    process = subprocess.Popen([executable, '-e', source], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert process.stdout.readline() == b'ready\n', 'interpreter startup failed'
        def cpu_ticks():
            fields = open('/proc/%d/stat' % process.pid).read().split()
            return int(fields[13]) + int(fields[14])
        before = cpu_ticks()
        time.sleep(0.3)
        used = (cpu_ticks() - before) / os.sysconf('SC_CLK_TCK')
        assert used < 0.1, '%s idle CPU time %s' % (mode, used)
        start = time.monotonic()
        process.send_signal(signal.SIGINT)
        output, error = process.communicate(timeout=2.5)
        latency = time.monotonic() - start
        assert process.returncode != 0 and b'interrupted' in error, error.decode()
        print('%s: first SIGINT %.3fs, idle CPU %.3fs' % (mode, latency, used))
    finally:
        if process.poll() is None:
            process.kill()
            process.communicate()
    repeated = subprocess.Popen([executable, '-e', source], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        assert repeated.stdout.readline() == b'ready\n'
        time.sleep(0.1)
        repeated.send_signal(signal.SIGINT)
        time.sleep(0.03)
        if repeated.poll() is None:
            repeated.send_signal(signal.SIGINT)
        output, error = repeated.communicate(timeout=2.5)
        assert repeated.returncode == -signal.SIGINT or b'interrupted' in error, error.decode()
        print('%s: repeated SIGINT exit %s' % (mode, repeated.returncode))
    finally:
        if repeated.poll() is None:
            repeated.kill()
            repeated.communicate()
