import platform as _platform

name = "preftomotion"
version = "1.0.0"
authors = ["Marten Blumen"]
description = "PRefToMotion - KD-tree PRef-to-STMap solver (optimized fork)."

# One variant per Nuke major.minor you ship. rez builds + installs each
# separately, so the ABI-matched .dll is selected at runtime by the nuke
# version in the resolve.
variants = [
    ["nuke-14.1"],
    ["nuke-15.1"],
    ["nuke-16.1"],
]

# Need cmake in the build resolve. Drop this if cmake is on system PATH instead
# of being a rez package.
private_build_requires = ["cmake"]

# Build via the Visual Studio generator. MSBuild finds the MSVC toolchain itself
# (registry/vswhere), so this needs NO vcvars and NO nmake in the rez context -
# which is why launching from a VS dev prompt wasn't helping. See rezbuild.* .
if _platform.system() == "Windows":
    build_command = "{root}\\rezbuild.bat {install}"
else:
    build_command = "bash {root}/rezbuild.sh {install}"

def commands():
    # the built .dll is installed under <variant root>/nuke
    env.NUKE_PATH.append("{root}/nuke")
