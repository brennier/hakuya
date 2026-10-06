# Hakuya is a PlayStation 1 Emulator

Hakuya is a PlayStation 1 emulator written in strictly conformant C99. This is a personal project to learn more about computers, emulation, and graphics programming. This project strives to use as few libraries and extensions as possible. The flags `-std=c99 -pedantic-errors` are used to ensure that no GCC/Clang extensions are used.

<details>
  <summary><h3>Progress Report (click here to expand)</h3></summary>

- [ ] Overall
  - [x] Basic TTY output
  - [x] Support for running EXE files
- [ ] CPU
  - [x] Opcode parsing
  - [x] Opcode implementation (passes all of 101 of AmiDog's CPU tests)
  - [x] Branch delays and load delays
  - [x] Exceptions
  - [ ] Cycle timing
- [ ] GPU (software rendering)
- [ ] GPU (hardware rendering)
- [ ] DMA
- [ ] SPU
- [ ] CDROM
</details>

<details>
  <summary><h3>Resources (click here to expand)</h3></summary>

- [Nocash PSXSPX Playstation Specifications](https://problemkaputt.de/psxspx-contents.htm)
This is the holy grail of references. Very comprehensive, very terse. Primarily useful for looking up tables and codes.

- [MIPS R30xx Software Reference Manual](https://cgi.cse.unsw.edu.au/~cs3231/doc/R3000.pdf)
The PS1 uses a modified R3000 CPU. This manual explains a lot about how this architecture is designed. I found it particularly good for explaining Exceptions/Interrupts.

- [MIPS32 Instruction Set Reference](https://hades.mech.northwestern.edu/images/1/16/MIPS32_Architecture_Volume_II-A_Instruction_Set.pdf)
A good reference on all of the MIPS opcodes. Simply look up the name of the opcode and read a 1 to 2 page overview of what the opcode does, what are some of the edge cases of the opcode, and how it differs from other similar opcodes.

- [jsgroth's blog (CPU, basic GPU, and SPU)](https://jsgroth.dev/blog/posts/ps1-cpu/)
Great blog that I also used when programming my gameboy emulator. There's several articles on his blog, including:
  - A high-level overview of MIPS CPU and some of its traps and pitfalls
  - A good article on how to quickly get TTY output and EXE sideloading working
  - Two articles on a basic implementation of the GPU
  - Several articles on the SPU (I still haven't read these, though)

- [Simias Playstation Emulation Guide (CPU, DMA)](https://vojty.github.io/psx-guide/guide.pdf)
This guide is a step-by-step walkthrough of building a PS1 emulator. There are many, many code snippets showing exactly how he implements each step. It goes through all of the CPU opcodes, basic DMA transfers, and a basic implementation of a GPU using OpenGPL. The goal of the guide is to boot the BIOS. It's unfortunately unfinished and stops right after getting the orange diamond to display.

- [AmiDog's CPU test](https://psx.amidog.se/doku.php?id=psx:download:cpu)
A comprehensive EXE file for testing the functionality and accuracy of the CPU. Covers quite a few edges cases regarding load delays, branch delays, half loads, undefined divisions, overflow exceptions, etc. It prints results to the TTY as well as the screen, so no GPU is needed.

- [Peter Lemon test roms](https://github.com/PeterLemon/PSX/tree/master)
A suite of small EXE files for testing individual parts of the emulator. It's particularly good for testing all of the types of GPU draw instructions.

</details>
