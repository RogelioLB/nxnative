"""dm/android/spv_readonly.h (NonWritable on the storage resources of vertex-pipeline shaders, for GPUs without vertexPipelineStoresAndAtomics: the Mali-G76),
built for the PC with the Android NDK's clang and run on modules assembled with its spirv-as; spirv-val checks the result. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every vertex shader there (each result must pass spirv-val)."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]


def ndk():
    for c in (os.environ.get('ANDROID_NDK_ROOT'), *(Path(s) / 'ndk' for s in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'),
              Path(os.environ.get('LOCALAPPDATA', '')) / 'Android' / 'Sdk') if s)):
        if c and Path(c).is_dir():
            vs = sorted(Path(c).iterdir()) if not (Path(c) / 'shader-tools').is_dir() else [Path(c)]
            for v in reversed(vs):
                if (v / 'shader-tools').is_dir(): return v
    return None


NDK = ndk()
HOST = 'windows-x86_64' if os.name == 'nt' else 'linux-x86_64' if os.uname().sysname == 'Linux' else 'darwin-x86_64'
EXE = '.exe' if os.name == 'nt' else ''

VS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %pos
OpDecorate %pos BuiltIn Position
OpDecorate %ro DescriptorSet 0
OpDecorate %ro Binding 0
OpDecorate %rw DescriptorSet 0
OpDecorate %rw Binding 1
OpDecorate %buf BufferBlock
OpMemberDecorate %buf 0 Offset 0
OpDecorate %arr ArrayStride 16
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%v4 = OpTypeVector %f32 4
%arr = OpTypeRuntimeArray %v4
%buf = OpTypeStruct %arr
%pbuf = OpTypePointer Uniform %buf
%ro = OpVariable %pbuf Uniform
%rw = OpVariable %pbuf Uniform
%pv4 = OpTypePointer Uniform %v4
%pout = OpTypePointer Output %v4
%pos = OpVariable %pout Output
%i32 = OpTypeInt 32 1
%c0 = OpConstant %i32 0
%main = OpFunction %void None %fn
%l = OpLabel
%a = OpAccessChain %pv4 %ro %c0 %c0
%v = OpLoad %v4 %a
OpStore %pos %v
%b = OpAccessChain %pv4 %rw %c0 %c0
OpStore %b %v
OpReturn
OpFunctionEnd
'''


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvReadonly(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        bin_ = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 'r.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_readonly.h"\n{export} size_t run(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap, int *wr) {{ return spv_readonly(w, n, s, o, cap, wr); }}\n')
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        if os.name == 'nt':   # no C library: a DLL linked by lld alone
            subprocess.run([bin_ / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding', '-nostdlib', '-O1', '-Wall', '-I', REPO / 'dm/android', '-c', src, '-o', cls.tmp / 'r.obj'], check=True)
            subprocess.run([bin_ / 'ld.lld.exe', '-flavor', 'link', '-dll', '-noentry', f'-out:{cls.tmp / "r.dll"}', str(cls.tmp / 'r.obj')], check=True, env=env)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'r.dll'))
        else:
            subprocess.run([bin_ / 'clang', '-shared', '-fPIC', '-O1', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'r.so'], check=True)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'r.so'))
        cls.lib.run.restype = ctypes.c_size_t

    def run_on(self, data):
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data); bound = w[3]
        s = (ctypes.c_uint32 * (2 * bound))(); cap = n + 3 * bound; o = (ctypes.c_uint32 * cap)(); wr = ctypes.c_int()
        k = self.lib.run(w, n, s, o, cap, ctypes.byref(wr))
        return (bytes(o)[:4 * k] if k else None), wr.value

    def val(self, data, name):
        p = self.tmp / name; p.write_bytes(data)
        r = subprocess.run([self.spv_val, '--target-env', 'vulkan1.1', p], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def asm(self, text, name):
        p = self.tmp / f'{name}.spvasm'; p.write_text(text)
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', p, '-o', self.tmp / f'{name}.spv'], check=True)
        return (self.tmp / f'{name}.spv').read_bytes()

    def decorated(self, data):   # the ids decorated NonWritable
        import struct
        w = struct.unpack(f'<{len(data) // 4}I', data); i, out = 5, set()
        while i < len(w):
            if w[i] & 0xffff == 71 and w[i] >> 16 == 3 and w[i + 2] == 24: out.add(w[i + 1])
            i += w[i] >> 16
        return out

    def test_read_only_buffer_decorated_written_one_left(self):
        data = self.asm(VS, 'vs')
        out, wr = self.run_on(data)
        self.assertIsNotNone(out); self.assertEqual(wr, 1)
        self.assertEqual(len(self.decorated(out)), 1)
        self.val(out, 'vs_out.spv')

    def test_fragment_shader_left_alone(self):
        data = self.asm(VS.replace('OpEntryPoint Vertex %main "main" %pos', 'OpEntryPoint Fragment %main "main" %pos\nOpExecutionMode %main OriginUpperLeft')
                          .replace('OpDecorate %pos BuiltIn Position', 'OpDecorate %pos Location 0'), 'fs')
        self.assertEqual(self.run_on(data), (None, 0))

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR not set')
    def test_a_games_vertex_shaders(self):
        import json
        d = Path(os.environ['DM_SPV_DIR']); n = changed = written = 0
        for rj in sorted(d.glob('*.res.json')):
            if json.loads(rj.read_text(encoding='utf-8')).get('stage') != 'vs': continue
            for sp in (d / (rj.name[:-9] + '.spv'), d / (rj.name[:-9] + '.z.spv')):
                if not sp.is_file(): continue
                n += 1; out, wr = self.run_on(sp.read_bytes()); written += wr
                if out: changed += 1; self.val(out, 'game.spv')
        print(f'{n} vertex shader modules, {changed} decorated, {written} written resources left')
        self.assertTrue(n)


if __name__ == '__main__':
    unittest.main()
