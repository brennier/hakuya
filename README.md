# Hakuya is a PlayStation 1 Emulator

Hakuya is a PlayStation 1 emulator written in strictly conformant C99. This is a personal project to learn more about computers, emulation, and graphics programming. This project strives to use as few libraries and extensions as possible. The flags `-std=c99 -pedantic-errors` are used to ensure that no GCC/Clang extensions are used. At the moment, SDL3 is used for the windowing and software rendering. The plan is to use SDL3's new 3D API to implement the hardware renderer.

