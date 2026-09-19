import io
import os

base = r'C:\Users\Mastersell\AppData\Local\Temp\opencode\bl1gotyvr-b49-clean'
source_lines = io.open(base + r'\third_party\fsr1\ffx_fsr1.h',
                       encoding='utf-8', errors='replace').read().splitlines()
# Non-packed EASU section: '#if defined(A_GPU)&&defined(FSR_EASU_F)' .. '#endif'
guard_start = next(i for i, line in enumerate(source_lines)
                   if line.strip() == '#if defined(A_GPU)&&defined(FSR_EASU_F)')
guard_end = guard_start
depth = 0
for i in range(guard_start, len(source_lines)):
    stripped = source_lines[i].strip()
    if stripped.startswith('#if'):
        depth += 1
    elif stripped.startswith('#endif'):
        depth -= 1
        if depth == 0:
            guard_end = i
            break
code = '\n'.join(source_lines[guard_start:guard_end + 1])
assert 'FsrEasuTapF' in code and 'FsrEasuF(' in code
assert '"' not in code, 'unexpected quote in FSR source'

shim = """// FidelityFX FSR1 EASU (MIT, (c) 2021 Advanced Micro Devices, Inc.)
// Extracted verbatim from ffx_fsr1.h; only the type/math macros and the
// section guards the reference header expects are provided here.
#define A_GPU 1
#define FSR_EASU_F 1
#define AF1 float
#define AF2 float2
#define AF3 float3
#define AF4 float4
#define AU2 uint2
#define AU4 uint4
#define AP1 bool
#define AF1_(a) (float(a))
#define AF2_(a) (float2(a, a))
#define AF3_(a) (float3(a, a, a))
#define AF4_(a) (float4(a, a, a, a))
#define AF2_AU2(a) (asfloat(a))
#define ASatF1(a) (saturate(a))
#define ARcpF1(a) (rcp(a))
#define APrxLoRcpF1(a) (rcp(a))
#define APrxLoRsqF1(a) (rsqrt(a))
#define AMin3F3(a, b, c) (min(a, min(b, c)))
#define AMax3F3(a, b, c) (max(a, max(b, c)))
#define A_STATIC static
"""

header = ('// Generated from ffx_fsr1.h (MIT). Do not edit by hand.\n'
          '#pragma once\n\n'
          'static const char kFsrEasuSource[] = R"FSR(\n'
          + shim + code + '\n)FSR";\n')
io.open(base + r'\src\xr\FsrEasuShaderSource.h', 'w', encoding='utf-8',
        newline='\r\n').write(header)
inc = base + r'\src\xr\FsrEasuShader.inc'
if os.path.exists(inc):
    os.remove(inc)
print('generated FsrEasuShaderSource.h', len(header), 'bytes')
