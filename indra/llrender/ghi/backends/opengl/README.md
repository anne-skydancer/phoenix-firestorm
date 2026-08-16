# OpenGL GHI backend

R1 provides the Windows OpenGL peer presentation adapter and renderer identity
capture here. New OpenGL entry points and native types are permitted in this
directory only. The lifecycle harness verifies clear/present, resize,
display-change notification, minimize/restore, teardown, and physical-GPU
identity parity with Vulkan.
