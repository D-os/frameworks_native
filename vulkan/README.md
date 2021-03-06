# frameworks/native/vulkan

This subdirectory contains Android's Vulkan loader, as well as some Vulkan-related tools useful to platform developers.

## Documentation

The former contents of doc/implementors_guide/ are now at https://source.android.com/devices/graphics/implement-vulkan.

## Coding Style

We follow the [Chromium coding style](https://www.chromium.org/developers/coding-style) for naming and formatting, except with four-space indentation instead of two spaces. In general, any C++ features supported by the prebuilt platform toolchain are allowed.

Use "clang-format -style=file" to format all C/C++ code, except code imported verbatim from elsewhere. Setting up git-clang-format in your environment is recommended.

## Code Generation

We generate several parts of the loader and tools directly from the Vulkan Registry (external/vulkan-headers/registry/vk.xml). Code generation must be done manually because the generator is not part of the platform toolchain (yet?). Files named `foo_gen.*` are generated by the code generator.

### Run The Code Generator

Install Python3 (if not already installed) and execute below:
`$ ./scripts/code_generator.py`
