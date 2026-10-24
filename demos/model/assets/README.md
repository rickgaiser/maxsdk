# Models

`Fox.glb`, from the Khronos glTF Sample Assets
(https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Fox), unchanged:

- model: PixelMannen, [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/legalcode)
- rigging and animation: tomkranis, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode)
- conversion to glTF: @AsoboStudio and @scurest, [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode)

The Makefile converts it with `tools/model/gltf2mxm.py` and builds it into `model.elf`. Other
models are converted the same way and loaded from the host:
`ps2client execee host:model.elf host:<name>.mxm`.
