"""Audited compatibility fixes for the pinned TFLite 2.14 SDK and GCC 14.

Keep hashes tied to openwakeword-sources.json: dependency updates must review
these fixes instead of silently applying a patch to different upstream code.
No warning flags or inference arithmetic are changed here.
"""
import hashlib


PATCHES = (
    (
        'tensorflow/tensorflow/lite/kernels/cpu_backend_gemm_custom_gemv.h',
        '149f3680ef334e5cc5747cc20e0dfabb851294b947060681c9885e2b9da9dcbd',
        # The builder already requires C++17; use its portable attribute.
        '[[clang::fallthrough]]', '[[fallthrough]]', 8,
    ),
    (
        'tensorflow/tensorflow/lite/core/acceleration/configuration/nnapi_plugin.h',
        '37f19d1b7b463277a1e9e8498f4fd8e1fe8aca00b147158228827d3c9d440df8',
        # FlatBuffers Length() is a deprecated wrapper around size().
        '->Length()', '->size()', 5,
    ),
    (
        'cpuinfo/src/arm/linux/chipset.c',
        '2564829c239eb14a783c3ef520cec26955776438696616644240ce512d20b29d',
        # Match the caller's 9-byte revision buffer and the strnlen bound in
        # this function, instead of claiming it provides 64 hardware bytes.
        'const char revision[restrict static CPUINFO_HARDWARE_VALUE_MAX]',
        'const char revision[restrict static CPUINFO_REVISION_VALUE_MAX]', 1,
    ),
    (
        'ruy/ruy/pack_arm.h',
        '121ca94087d7af7550f2302882e6c289ad33580098e5114026ee11dc020704e9',
        '#endif  // (RUY_PLATFORM_NEON_64 || RUY_PLATFORM_NEON_32) && \\\n        // RUY_OPT(ASM)',
        '#endif  // (RUY_PLATFORM_NEON_64 || RUY_PLATFORM_NEON_32) && RUY_OPT(ASM)', 1,
    ),
)


def apply_patches(sources):
    pending = []
    for relative, expected_hash, before, after, count in PATCHES:
        path = sources / relative
        original = path.read_bytes()
        if hashlib.sha256(original).hexdigest() != expected_hash:
            raise ValueError('Compatibility patch source mismatch: ' + relative)
        if original.count(before.encode()) != count:
            raise ValueError('Compatibility patch occurrence mismatch: ' + relative)
        pending.append((path, original.replace(before.encode(), after.encode())))
    # Validate every input before changing any file.
    for path, patched in pending:
        path.write_bytes(patched)
        print('Applied SDK compatibility fix: ' + str(path.relative_to(sources)), flush=True)
